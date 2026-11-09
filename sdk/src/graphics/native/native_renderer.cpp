// DS2 port: experimental native renderer (v51). See native_renderer.h and docs/handoff/18-native-renderer.md.
//
// Copyright notes: the register/shader plumbing follows the SDK's Vulkan backend (Xenia, BSD-3, see
// src/graphics/vulkan); the overall approach (own graphics system, render targets as images, resolves as copies)
// follows nfsmw-nx (GPL-3, docs/native-renderer.md there). This file is new code.

#include "native_renderer.h"
#include <rex/graphics/native/bcn_decode.h>
#include <rex/graphics/native/upload_content_cache.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <pthread.h>
#include <thread>
#include <sys/stat.h>
#include <map>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/shader/native_vertex_layout.h>
#include <rex/graphics/pipeline/shader/spirv_library.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_INT32(ds2_native_shader_profile_every, 0, "GPU",
                     "Sample scene shader groups every N frames (0 disables); separate prepass/material/post budgets (192 total groups per frame)")
    .range(0, 1000000);
REXCVAR_DEFINE_BOOL(ds2_native_dynamic_ubo, false, "GPU",
                    "Reuse constant descriptor sets with per-draw dynamic offsets")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(ds2_native_resolve_image_barriers, false, "GPU",
                    "Use image-scoped dependencies around resolve copies");
REXCVAR_DEFINE_BOOL(ds2_native_resolve_logical_size, true, "GPU",
                    "Sample resolved textures at fetch dimensions, without guest pitch/resolve padding");
REXCVAR_DEFINE_BOOL(ds2_native_resolve_color_convert, true, "GPU",
                    "Convert single-sample float color resolves to RGBA8 with the guest copy exponent bias")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(ds2_native_fold_texture_signs, false, "GPU",
                    "RC96: texture signs/gamma as pipeline specialization constants (needs a pack built with "
                    "REX_FOLD_TEXTURE_SIGNS=1); the sign switch and the second sample fold away");
REXCVAR_DEFINE_BOOL(ds2_native_alpha_specialization, false, "GPU",
                    "Compile away alpha tests only when the guest comparison is Always");
REXCVAR_DEFINE_BOOL(ds2_native_cpu_cache, false, "GPU",
                    "Reuse exact immutable constant/index uploads and descriptor scratch storage within a frame");
REXCVAR_DEFINE_BOOL(ds2_native_vertex_inputs, false, "GPU",
                    "Use raw Vulkan vertex attributes for validated straight-line vertex fetch prefixes");
REXCVAR_DEFINE_BOOL(ds2_native_texture_sets, false, "GPU",
                    "Reuse immutable texture descriptor sets within a frame; false restores per-draw allocation");
REXCVAR_DEFINE_BOOL(ds2_native_renderer, false, "GPU",
                    "DS2 port (v51, experimental): draw with the native renderer instead of the Xenos emulation "
                    "(no EDRAM, render targets as images, resolves as image copies). Fallback: false")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(ds2_native_log_every, 300, "GPU",
                     "DS2 native renderer: frames between statistics lines in the log (0 = never)")
    .range(0, 100000);
REXCVAR_DEFINE_STRING(ds2_native_dump_dir, "", "GPU",
                      "DS2 native renderer (development): directory for PPM dumps of every render target and resolve "
                      "target, written once at swap ds2_native_dump_swap");
REXCVAR_DEFINE_INT32(ds2_native_dump_swap, 0, "GPU", "DS2 native renderer (development): dump at this frame counted among frames with more than 200 draws")
    .range(0, 100000000);
REXCVAR_DEFINE_INT32(ds2_native_texture_keep_frames, 600, "GPU",
                     "DS2 native renderer: release guest textures not sampled for this many frames")
    .range(30, 100000);
REXCVAR_DEFINE_BOOL(ds2_native_gamma_ramp, true, "GPU",
                    "DS2 native renderer: apply the game's 256-entry gamma ramp when presenting (as the emulation)");
REXCVAR_DEFINE_BOOL(ds2_native_textures_1d, true, "GPU",
                    "DS2 native renderer: sample 1D textures (false = white, as before RC63)");
REXCVAR_DEFINE_BOOL(ds2_native_clamp_7e3, true, "GPU",
                    "Clamp pixel shader color output for k_2_10_10_10_FLOAT render targets to the "
                    "Xenos 7e3 range [0, 31.875] (NaN -> 0), as the guest render target would store it");
REXCVAR_DEFINE_BOOL(ds2_native_7e3_as_r11g11b10, false, "GPU",
                    "DS2 native renderer (RC67 test): store k_2_10_10_10_FLOAT render targets as B10G11R11_UFLOAT "
                    "instead of RGBA16F (half the bandwidth, no alpha); restart");
REXCVAR_DEFINE_BOOL(ds2_native_resolve_depth_only, true, "GPU",
                    "Depth resolves copy only the depth plane (textures never sample the stencil of a resolve)");
REXCVAR_DEFINE_BOOL(ds2_native_state_filter, true, "GPU",
                    "Skip vkCmd* state calls (pipeline, descriptor sets, viewport, scissor, depth bias, blend "
                    "constants, stencil) that repeat what the command buffer already has");
REXCVAR_DEFINE_BOOL(ds2_native_barrier_elision, true, "GPU",
                    "DS2 native renderer: skip a pipeline barrier when no GPU work was recorded since the previous one "
                    "(each barrier is a GPU wait-for-idle on NVK)");
REXCVAR_DEFINE_BOOL(ds2_native_float24_half_range, false, "GPU",
                    "DS2 native renderer: map D24FS8 depth [0,2) to host [0,1) (before RC66). false keeps guest depth "
                    "values, so depth textures (shadow maps, fog/DOF/soft-particle depth) read what the game wrote");
REXCVAR_DEFINE_INT32(ds2_native_gpu_profile_every, 30, "GPU",
                     "DS2 native renderer: with ds2_native_gpu_profile, time one command buffer in N (timestamps "
                     "drain the GPU on NVK); 1 = every one, as before RC66")
    .range(1, 100000);
REXCVAR_DEFINE_BOOL(ds2_native_depth_from_rgba8, true, "GPU",
                    "DS2 native renderer: decode k_24_8/k_24_8_FLOAT fetches of 8888 depth resolves to R32F depth "
                    "(shadow maps, depth effects); false = raw bytes as before RC64");
REXCVAR_DEFINE_BOOL(ds2_native_textures_cube, true, "GPU",
                    "DS2 native renderer: sample cube textures (false = white, as before RC64)");
REXCVAR_DEFINE_DOUBLE(ds2_native_present_gamma, 1.0, "GPU",
                      "DS2 native renderer: display gamma after the guest ramp, out = in^(1/value); 1.0 = as the "
                      "Xbox 360 output, above 1.0 brightens dark tones (TV vs. handheld screen)")
    .range(0.5, 2.5);
REXCVAR_DEFINE_BOOL(ds2_native_textures, true, "GPU",
                    "DS2 native renderer: load guest textures (false = a white 1x1 texture everywhere, for "
                    "checking geometry)");
REXCVAR_DEFINE_BOOL(ds2_native_bcn_decode, true, "GPU",
                    "DS2 native renderer: on hosts without BCn (MoltenVK on iOS has no S3TC) CPU-decode guest DXT "
                    "textures to uncompressed texels on upload (a port of the Xenia DXT load shaders); false = "
                    "require host BCn (compressed textures are white on such hosts)");
REXCVAR_DEFINE_BOOL(ds2_native_gpu_profile, false, "GPU",
                    "DS2 native renderer (v54): GPU timestamps around every render pass, resolve, clear and upload; "
                    "the GPU time per frame and the most expensive pass kinds are logged with the statistics");
REXCVAR_DEFINE_BOOL(ds2_native_mips, true, "GPU",
                    "DS2 native renderer (v54): load the mip levels of guest textures (false = base level only, as "
                    "v51-v53)");
REXCVAR_DEFINE_INT32(ds2_native_max_anisotropy, 0, "GPU",
                     "DS2 native renderer (v54): anisotropic filtering cap (0/1 = off; the game's request is capped "
                     "to this)")
    .range(0, 16);
REXCVAR_DEFINE_BOOL(ds2_native_async_pipelines, true, "GPU",
                    "DS2 native renderer (v55): compile new pipelines on a worker thread; an object whose pipeline is "
                    "not ready is skipped for those frames instead of freezing the picture (full-screen passes still "
                    "wait)");
REXCVAR_DEFINE_INT32(ds2_native_hitch_ms, 100, "GPU",
                     "DS2 native renderer (v55): log what a frame spent its time on when it takes longer than this "
                     "(0 = never)")
    .range(0, 100000);
REXCVAR_DEFINE_BOOL(ds2_native_msaa_host, true, "GPU",
                    "RC71: draw 4x MSAA passes with color targets into real 4x MSAA images (pixel shaders once per "
                    "pixel as on the Xbox 360) instead of the 2x2 sample-space image; false = sample space as before");
REXCVAR_DEFINE_INT32(ds2_native_msaa_min_draws, 8, "GPU",
                     "RC72: a 4x color target uses the host 4x copy only with at least this many non-rectangle 4x "
                     "draws in this or the previous frame (DS2 clears in 4x and draws at 1x elsewhere)");
REXCVAR_DEFINE_BOOL(ds2_native_shadow_face_clear, true, "GPU",
                    "RC74: clear the whole viewport of a shadow face drawn after a depth-only clear rectangle");
REXCVAR_DECLARE(std::string, ds2_shader_library);
REXCVAR_DEFINE_STRING(ds2_native_halfres_ps,
                      "70FC5FED5D6D2178 73889BBD9480721F 69CF96E66B5F2822 7FE96CCABD9DF34E F2DDD0716A037207 "
                      "50455868F53A23E3:z 6A6C52E883DEE5FA:z", "GPU",
                      "RC82: pixel shaders (hex hashes) of additive particle draws drawn at half resolution and added "
                      "to the target (bonfire fog); empty = off");
REXCVAR_DEFINE_DOUBLE(ds2_native_halfres_strength, 0.75, "GPU",
                      "RC83: brightness of the half-resolution particles when added (bonfire fog); 1 = as the game");
REXCVAR_DEFINE_INT32(ds2_native_halfres_divisor, 2, "GPU",
                     "RC84: resolution divisor of the reduced-resolution particles (2 = half, 4 = quarter)");
REXCVAR_DEFINE_INT32(ds2_native_halfres_vpos_const, 215, "GPU",
                     "RC82: pixel shader float constant whose zw scale the pixel position (doubled for half resolution)");
REXCVAR_DEFINE_BOOL(ds2_native_halfres_msaa, true, "GPU",
                    "v105: reduced-resolution particles also in host 4x MSAA passes (the main scene: fog and smoke "
                    "drawn there cost up to 27 ms at full resolution); composited into the 4x copy");
REXCVAR_DEFINE_BOOL(ds2_native_halfres_alpha_over, true, "GPU",
                    "v105: reduced-resolution particles also accept blend 07060706 (alpha = src_a * src_a + ...); "
                    "the copy keeps what remains of the scene in alpha, as for 07000706");
REXCVAR_DEFINE_BOOL(ds2_native_resolve_skip_unchanged, true, "GPU",
                    "v106: a resolve copies nothing when its source was not written since the same copy into the "
                    "same unchanged texture (DS2 resolves its depth buffer several times per frame)");
REXCVAR_DEFINE_INT32(ds2_fps_cap, 0, "GPU",
                     "v106: frame rate cap with even frame spacing (30 = a steady 30 instead of 28-40 jumping); "
                     "0 = off");
REXCVAR_DEFINE_DOUBLE(ds2_render_scale, 1.0, "GPU",
                      "v107: internal resolution of the scene (1 = 720p, 0.75 = 540p); chosen on the start screen "
                      "when ds2_resolution_prompt is on");
REXCVAR_DEFINE_BOOL(ds2_native_convert_written_rows, true, "GPU",
                    "v109: an EDRAM range taken over by another format converts only the rows written since the "
                    "previous owner took it (bottom strip of the screen blurred by depth of field before)");
REXCVAR_DEFINE_BOOL(ds2_fps_lock_vsync, false, "GPU",
                    "v116: with ds2_fps_cap = 30, present on every other display refresh (swap interval 2); v118: "
                    "off by default (a frame over 33 ms waits for 50 ms: hitches)");
REXCVAR_DEFINE_BOOL(ds2_sharpen, true, "GPU",
                    "v116: at a reduced internal resolution, upscale to 1280x720 with contrast-adaptive sharpening "
                    "(chosen on the start screen) instead of the presenter's bilinear filter");
REXCVAR_DEFINE_DOUBLE(ds2_sharpness, 0.6, "GPU", "v116: strength of the sharpening, 0..1");
REXCVAR_DEFINE_BOOL(ds2_native_dynamic_textures, true, "GPU",
                    "RC86: a texture whose content changed is checked every frame for 120 frames (videos)");
REXCVAR_DEFINE_BOOL(ds2_native_fast_registers, true, "GPU",
                    "RC81: plain register writes only store the value (no register info lookup and side-effect switch)");
REXCVAR_DEFINE_BOOL(ds2_native_texture_scratch, true, "GPU",
                    "RC80: build texture rows in cached memory, then write them once to the uncached upload buffer");
REXCVAR_DEFINE_INT32(ds2_native_gpu_spike_ms, 60, "GPU",
                    "RC80: after a frame that waited 40+ ms for the GPU, time every command buffer with shader groups "
                    "and log those that took this many ms (0 = off)");
REXCVAR_DEFINE_BOOL(ds2_native_async_shader_restore, true, "GPU",
                    "RC79: restore shaders from the pack on a worker; draws wait for them (skipped) instead of the frame");
REXCVAR_DEFINE_BOOL(ds2_native_constant_reuse, true, "GPU",
                    "RC78: a constant buffer equal to the previous draw's reuses its upload");
REXCVAR_DEFINE_BOOL(ds2_native_persistent_texture_sets, true, "GPU",
                    "RC77: keep texture descriptor sets across frames (dropped when a view they use is destroyed)");
REXCVAR_DEFINE_BOOL(ds2_native_fast_untile, true, "GPU",
                    "RC77: untile 2D textures 16 bytes at a time with the offset computed inline");
REXCVAR_DEFINE_BOOL(ds2_native_pipeline_steal, true, "GPU",
                    "RC77: a pipeline needed at once that is still queued for the worker is created on the command "
                    "thread instead of waiting for the whole queue");
REXCVAR_DEFINE_BOOL(ds2_native_depth_bias_absolute, true, "GPU",
                    "RC76: absolute constant depth bias as on the Xenos (VK_EXT_depth_bias_control unorm24 units), "
                    "polygon offset of the drawn face; false = RC75 (relative to the depth exponent, front only)");
REXCVAR_DEFINE_BOOL(ds2_native_resolve_float24, true, "GPU",
                    "RC76: D24FS8 depth resolves hold float24 depth (truncated) as on the console, not the float32 "
                    "host depth (point-light shadow acne: striped squares on torch-lit walls)");
REXCVAR_DEFINE_BOOL(ds2_native_float24_round, false, "GPU",
                    "RC76: round float24 depth to nearest even in depth resolves (false = truncate, as the emulation)");
REXCVAR_DEFINE_BOOL(ds2_native_msaa_clear_replay, true, "GPU",
                    "RC73: draw lone 4x clear rectangles in sample space and replay them into the 4x copies; false = "
                    "RC71 (4x clears go to the 4x copies)");
REXCVAR_DEFINE_BOOL(ds2_native_bias_sample_scale, false, "GPU",
                    "RC72: scale the slope-scaled depth bias by the sample-space factor of 4x targets");
REXCVAR_DEFINE_INT32(ds2_native_shader_profile_groups, 0, "GPU",
                     "RC72: shader groups timed per profiled command buffer (0 = 64/96/32 by pass kind)");
REXCVAR_DEFINE_BOOL(ds2_native_msaa_skip_clear_import, true, "GPU",
                    "RC71: a 4x pass that starts with a full clear draw does not import the previous contents");
REXCVAR_DEFINE_BOOL(ds2_native_resolve_clip_to_dest, false, "GPU",
                    "RC70: clip a 1x resolve rectangle to the destination texture size (no logical-size copies)");
REXCVAR_DEFINE_BOOL(ds2_native_capture_resolves, true, "GPU",
                    "RC70: during an L3+R3 capture also write every color resolve at its point in the frame (snap_*)");
REXCVAR_DEFINE_BOOL(ds2_native_capture_combo, true, "GPU",
                    "DS2 native renderer (v55, Switch): holding L3 + R3 (both sticks pressed) for ~1 s traces the "
                    "next in-game frame into the log and writes its render targets as images under "
                    "sdmc:/switch/ds2/native_dump (or ds2_native_dump_dir)");
REXCVAR_DEFINE_BOOL(ds2_native_pass_barriers, false, "GPU",
                    "DS2 native renderer (v55): full barrier before every render pass (true = as v51-v54; for "
                    "checking if something renders wrong without it)");
REXCVAR_DEFINE_BOOL(ds2_native_early_z, true, "GPU",
                    "DS2 native renderer (v55): early depth test for pixel shaders without kill / depth output / "
                    "alpha-dependent coverage (the emulation does the same)");
REXCVAR_DEFINE_BOOL(ds2_native_occlusion_queries, false, "GPU",
                    "DS2 native renderer (v54): real occlusion queries (Vulkan), the result reaches the game a few "
                    "frames late. false = a fixed 'visible' result as in the emulation (query_occlusion_fake_sample_"
                    "count): glows and flares then show through walls");

REXCVAR_DEFINE_INT32(ds2_native_occlusion_hold, 2, "GPU",
                     "RC69: an occlusion query reports 'hidden' only after this many consecutive zero measurements "
                     "(otherwise the largest recent count), so a late result cannot make an object blink; 1 = raw");
REXCVAR_DECLARE(int32_t, query_occlusion_fake_sample_count);
REXCVAR_DEFINE_INT32(ds2_native_probe, 0, "GPU",
                     "RC90 diagnostic probe, the picture is wrong on purpose: 1 = game draws with an empty scissor (vertex "
                     "work only, no pixels), 2 = only the first triangle of every game draw (pixel work only), 3 (RC91) "
                     "= every game pixel shader replaced by one that writes a constant (no shader math, no textures: "
                     "what is left is writing the targets); the probe is on in every other pair of log intervals, so "
                     "one session gives both numbers; 0 = off")
    .range(0, 3);

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfCount(unsigned id);  // switch_perf.cpp: 0 = swaps (FPS for the overlay / SaltyNX)
extern "C" int RexSwitchStickClicksHeld(void);     // switch_perf.cpp: L3 and R3 both held (handheld or pad 1)
#endif

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfIntervaloSwap(unsigned vblanks) __attribute__((weak));  // ui/switch_perf.cpp
#endif

namespace rex::graphics::vulkan {
extern std::atomic<bool> g_fragcoord_scale_patch;  // shader.cpp (v107)
constexpr uint32_t kFragCoordScaleSpecId = 0x5400;  // same value as in shader.cpp
}  // namespace rex::graphics::vulkan

namespace rex::graphics::native {

bool NativeRendererRequested() { return REXCVAR_GET(ds2_native_renderer); }

namespace {

using ui::vulkan::VulkanDevice;
using VulkanShader = vulkan::VulkanShader;

constexpr uint32_t kFrameSlots = 3;
constexpr VkDeviceSize kUploadBufferSize = VkDeviceSize(48) << 20;
static_assert(kUploadBufferSize <= UINT32_MAX, "Dynamic UBO offsets must fit in uint32_t");
constexpr uint32_t kMaxSetsPerFrame = 12288;

// glslang default built-in resource limits (copy of the Vulkan backend's).
constexpr TBuiltInResource kGlslangDefaultTBuiltInResource = {
    /* .maxLights = */ 32,
    /* .maxClipPlanes = */ 6,
    /* .maxTextureUnits = */ 32,
    /* .maxTextureCoords = */ 32,
    /* .maxVertexAttribs = */ 64,
    /* .maxVertexUniformComponents = */ 4096,
    /* .maxVaryingFloats = */ 64,
    /* .maxVertexTextureImageUnits = */ 32,
    /* .maxCombinedTextureImageUnits = */ 80,
    /* .maxTextureImageUnits = */ 32,
    /* .maxFragmentUniformComponents = */ 4096,
    /* .maxDrawBuffers = */ 32,
    /* .maxVertexUniformVectors = */ 128,
    /* .maxVaryingVectors = */ 8,
    /* .maxFragmentUniformVectors = */ 16,
    /* .maxVertexOutputVectors = */ 16,
    /* .maxFragmentInputVectors = */ 15,
    /* .minProgramTexelOffset = */ -8,
    /* .maxProgramTexelOffset = */ 7,
    /* .maxClipDistances = */ 8,
    /* .maxComputeWorkGroupCountX = */ 65535,
    /* .maxComputeWorkGroupCountY = */ 65535,
    /* .maxComputeWorkGroupCountZ = */ 65535,
    /* .maxComputeWorkGroupSizeX = */ 1024,
    /* .maxComputeWorkGroupSizeY = */ 1024,
    /* .maxComputeWorkGroupSizeZ = */ 64,
    /* .maxComputeUniformComponents = */ 1024,
    /* .maxComputeTextureImageUnits = */ 16,
    /* .maxComputeImageUniforms = */ 8,
    /* .maxComputeAtomicCounters = */ 8,
    /* .maxComputeAtomicCounterBuffers = */ 1,
    /* .maxVaryingComponents = */ 60,
    /* .maxVertexOutputComponents = */ 64,
    /* .maxGeometryInputComponents = */ 64,
    /* .maxGeometryOutputComponents = */ 128,
    /* .maxFragmentInputComponents = */ 128,
    /* .maxImageUnits = */ 8,
    /* .maxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .maxCombinedShaderOutputResources = */ 8,
    /* .maxImageSamples = */ 0,
    /* .maxVertexImageUniforms = */ 0,
    /* .maxTessControlImageUniforms = */ 0,
    /* .maxTessEvaluationImageUniforms = */ 0,
    /* .maxGeometryImageUniforms = */ 0,
    /* .maxFragmentImageUniforms = */ 8,
    /* .maxCombinedImageUniforms = */ 8,
    /* .maxGeometryTextureImageUnits = */ 16,
    /* .maxGeometryOutputVertices = */ 256,
    /* .maxGeometryTotalOutputComponents = */ 1024,
    /* .maxGeometryUniformComponents = */ 1024,
    /* .maxGeometryVaryingComponents = */ 64,
    /* .maxTessControlInputComponents = */ 128,
    /* .maxTessControlOutputComponents = */ 128,
    /* .maxTessControlTextureImageUnits = */ 16,
    /* .maxTessControlUniformComponents = */ 1024,
    /* .maxTessControlTotalOutputComponents = */ 4096,
    /* .maxTessEvaluationInputComponents = */ 128,
    /* .maxTessEvaluationOutputComponents = */ 128,
    /* .maxTessEvaluationTextureImageUnits = */ 16,
    /* .maxTessEvaluationUniformComponents = */ 1024,
    /* .maxTessPatchComponents = */ 120,
    /* .maxPatchVertices = */ 32,
    /* .maxTessGenLevel = */ 64,
    /* .maxViewports = */ 16,
    /* .maxVertexAtomicCounters = */ 0,
    /* .maxTessControlAtomicCounters = */ 0,
    /* .maxTessEvaluationAtomicCounters = */ 0,
    /* .maxGeometryAtomicCounters = */ 0,
    /* .maxFragmentAtomicCounters = */ 8,
    /* .maxCombinedAtomicCounters = */ 8,
    /* .maxAtomicCounterBindings = */ 1,
    /* .maxVertexAtomicCounterBuffers = */ 0,
    /* .maxTessControlAtomicCounterBuffers = */ 0,
    /* .maxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .maxGeometryAtomicCounterBuffers = */ 0,
    /* .maxFragmentAtomicCounterBuffers = */ 1,
    /* .maxCombinedAtomicCounterBuffers = */ 1,
    /* .maxAtomicCounterBufferSize = */ 16384,
    /* .maxTransformFeedbackBuffers = */ 4,
    /* .maxTransformFeedbackInterleavedComponents = */ 64,
    /* .maxCullDistances = */ 8,
    /* .maxCombinedClipAndCullDistances = */ 8,
    /* .maxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .maxDualSourceDrawBuffersEXT = */ 1,
    /* .limits = */
    {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    },
};


// Format reinterpretation passes (the few EDRAM reinterpretations DS2 relies on), compiled at startup with glslang.
enum ConvertMode : uint32_t {
  kConvertDepthToRgba8,      // D24S8 / D24FS8 -> the 32-bit guest word (depth << 8 | stencil) as RGBA8 bytes
  kConvertFloat7e3ToUnorm10, // k_2_10_10_10_FLOAT (RGBA16F image) -> its bits as k_2_10_10_10 (A2B10G10R10)
  kConvertUnorm10ToFloat7e3, // the reverse
  kConvertResolveColorRgba8, // numeric float color resolve, including copy exponent bias and UNORM8 packing
  kConvertRgba8ToDepth,      // RC64: guest 32-bit depth word stored as RGBA8 -> R32F depth for k_24_8(_FLOAT) fetches
  kConvertModeCount,
};

constexpr const char* kConvertVertexShader = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kConvertHeader = R"(#version 450
layout(push_constant) uniform P { ivec2 src_minus_dst; int float24; int exp_bias; } pc;
layout(location = 0) out vec4 o;
uint E7(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = floatBitsToUint(min(f, 31.875));
  if (u < 0x3E800000u) {
    uint sh = min(125u - (u >> 23), 24u);
    u = (0x800000u | (u & 0x7FFFFFu)) >> sh;
  } else {
    u += 0xC2000000u;
  }
  return ((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16) & 0x3FFu;
}
float D7(uint f) {
  f &= 0x3FFu;
  if (f == 0u) return 0.0;
  uint m = f & 0x7Fu, e = f >> 7;
  if (e == 0u) {
    uint lz = 31u - uint(findMSB(m)) - 24u;
    e = uint(1 - int(lz));
    m = (m << lz) & 0x7Fu;
  }
  return uintBitsToFloat(((e + 124u) << 23) | (m << 16));
}
uint F24(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = floatBitsToUint(f);
  if (u >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (u < 0x38800000u) {
    uint sh = min(113u - (u >> 23), 24u);
    u = (0x800000u | (u & 0x7FFFFFu)) >> sh;
  } else {
    u += 0xC8000000u;
  }
  // RC76: float24 = 2 truncates (toward zero, as the emulation's default depth_float24_round = false), 1 rounds to
  // nearest even. Rounding up puts half of the stored shadow depths above the surface's own depth, so a shadow
  // compare without a slope bias (DS2 point-light cube faces, sge in PS E94A8C3A) shadows the lit surface itself:
  // stripes in a square on torch-lit walls, flickering as the light moves.
  if (pc.float24 == 1) u += 3u + ((u >> 3) & 1u);
  return (u >> 3) & 0xFFFFFFu;
}
)";

constexpr const char* kConvertBodies[kConvertModeCount] = {
    R"(layout(set = 0, binding = 0) uniform sampler2D d;
layout(set = 0, binding = 1) uniform usampler2D s;
void main() {
  ivec2 c = ivec2(gl_FragCoord.xy) + pc.src_minus_dst;
  float z = texelFetch(d, c, 0).r;
  uint st = texelFetch(s, c, 0).r;
  // round(), not + 0.5: 16777215.5 is not a float32 and would become 2^24 (overflowing the depth bits).
  uint z24 = pc.float24 != 0 ? F24(z) : min(uint(round(clamp(z, 0.0, 1.0) * 16777215.0)), 0xFFFFFFu);
  uint v = (z24 << 8) | (st & 0xFFu);
  o = vec4(float(v & 0xFFu), float((v >> 8) & 0xFFu), float((v >> 16) & 0xFFu), float(v >> 24)) / 255.0;
}
)",
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
void main() {
  vec4 c = texelFetch(t, ivec2(gl_FragCoord.xy) + pc.src_minus_dst, 0);
  o = vec4(float(E7(c.r)) / 1023.0, float(E7(c.g)) / 1023.0, float(E7(c.b)) / 1023.0, clamp(c.a, 0.0, 1.0));
}
)",
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
void main() {
  vec4 c = texelFetch(t, ivec2(gl_FragCoord.xy) + pc.src_minus_dst, 0);
  uvec3 b = uvec3(c.rgb * 1023.0 + 0.5);
  o = vec4(D7(b.r), D7(b.g), D7(b.b), c.a);
}
)",
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
void main() {
  vec4 c = texelFetch(t, ivec2(gl_FragCoord.xy) + pc.src_minus_dst, 0);
  // Match the SDK full resolve: scale all RGBA, clamp, round half upward, pack UNORM8.
  // Source render-target color_exp_bias was already applied by the guest draw.
  c = clamp(c * exp2(float(pc.exp_bias)), 0.0, 1.0);
  o = floor(c * 255.0 + 0.5) / 255.0;
}
)",
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
// The guest word is (depth24 << 8) | stencil; kConvertDepthToRgba8 stored it as R = bits 0-7 ... A = bits 24-31.
float DF24(uint f) {
  f &= 0xFFFFFFu;
  if (f == 0u) return 0.0;
  uint m = f & 0xFFFFFu, e = f >> 20;
  if (e == 0u) {
    uint lz = 31u - uint(findMSB(m)) - 11u;
    e = uint(1 - int(lz));
    m = (m << lz) & 0xFFFFFu;
  }
  return uintBitsToFloat(((e + 112u) << 23) | (m << 3));
}
void main() {
  vec4 c = texelFetch(t, ivec2(gl_FragCoord.xy) + pc.src_minus_dst, 0);
  uvec4 b = uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
  uint z24 = (b.a << 16) | (b.b << 8) | b.g;
  float z = pc.float24 != 0 ? DF24(z24) : float(z24 + (z24 >> 23)) * (1.0 / 16777216.0);
  o = vec4(z);
}
)",
};

constexpr const char* kPresentShader = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(set = 0, binding = 1) uniform Ramp { uvec4 lut[64]; } ramp;
layout(push_constant) uniform P {
  vec2 scale; int use_ramp; float inv_gamma;
  // v116: upscale of a reduced internal resolution with contrast-adaptive sharpening (FSR 1 / CAS style).
  vec2 src_max; int sharpen; float sharpness;
} pc;
layout(location = 0) out vec4 o;
vec3 Bilinear(vec2 out_pos) {
  vec2 p = out_pos * pc.scale - 0.5;
  vec2 f = fract(p);
  ivec2 i = ivec2(floor(p));
  ivec2 m = ivec2(pc.src_max);
  vec3 a = texelFetch(t, clamp(i, ivec2(0), m), 0).rgb;
  vec3 b = texelFetch(t, clamp(i + ivec2(1, 0), ivec2(0), m), 0).rgb;
  vec3 c = texelFetch(t, clamp(i + ivec2(0, 1), ivec2(0), m), 0).rgb;
  vec3 d = texelFetch(t, clamp(i + ivec2(1, 1), ivec2(0), m), 0).rgb;
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
void main() {
  vec4 c;
  if (pc.sharpen != 0) {
    vec2 q = gl_FragCoord.xy;
    vec3 e = Bilinear(q);
    vec3 n = Bilinear(q + vec2(0.0, -1.0)), s = Bilinear(q + vec2(0.0, 1.0));
    vec3 w = Bilinear(q + vec2(-1.0, 0.0)), x = Bilinear(q + vec2(1.0, 0.0));
    vec3 mn = min(e, min(min(n, s), min(w, x)));
    vec3 mx = max(e, max(max(n, s), max(w, x)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1.0 / 65536.0)), 0.0, 1.0));
    vec3 wt = -amp * mix(0.125, 0.2, pc.sharpness);
    c = vec4(clamp((e + wt * (n + s + w + x)) / (1.0 + 4.0 * wt), 0.0, 1.0), 1.0);
  } else {
    c = texelFetch(t, ivec2(gl_FragCoord.xy * pc.scale), 0);
  }
  if (pc.use_ramp != 0) {
    uvec3 i = uvec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5);
    uint r = ramp.lut[i.r >> 2][i.r & 3u];
    uint g = ramp.lut[i.g >> 2][i.g & 3u];
    uint b = ramp.lut[i.b >> 2][i.b & 3u];
    c.rgb = vec3(float((r >> 20) & 1023u), float((g >> 10) & 1023u), float(b & 1023u)) / 1023.0;
  }
  if (pc.inv_gamma != 1.0) c.rgb = pow(clamp(c.rgb, 0.0, 1.0), vec3(pc.inv_gamma));
  o = vec4(c.rgb, 1.0);
}
)";

bool CompileGlsl(EShLanguage stage, const std::string& source, std::vector<uint32_t>& spirv, std::string& error) {
  static std::once_flag once;
  std::call_once(once, []() { glslang::InitializeProcess(); });
  const char* src = source.c_str();
  glslang::TShader shader(stage);
  shader.setStrings(&src, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kGlslangDefaultTBuiltInResource, 450, false, messages)) {
    error = shader.getInfoLog() ? shader.getInfoLog() : "parse failed";
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    error = program.getInfoLog() ? program.getInfoLog() : "link failed";
    return false;
  }
  glslang::SpvOptions options = {};
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
  return !spirv.empty();
}

struct NativeImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;  // full view (color, or depth + stencil for attachments)
  VkImageView depth_view = VK_NULL_HANDLE;    // depth aspect only, for sampling
  VkImageView stencil_view = VK_NULL_HANDLE;  // stencil aspect only, for sampling
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  uint32_t levels = 1;
  uint32_t layers = 1;  // 6 for a cube
  bool cube = false;
  uint32_t volume_depth = 0;  // v104: > 0 for a 3D image (VK_IMAGE_TYPE_3D)
  bool depth = false;
  uint32_t samples = 1;      // RC71: 4 for the host MSAA copies of 4x render targets
  uint64_t write_id = 0;     // v106: changes on every GPU write recorded into this image (0 = unknown)
  bool scaled = false;       // v107: a render target at the reduced internal resolution (ds2_render_scale)
  // v109: EDRAM ownership. Rows written since this image last became the owner of its EDRAM range (draws, clears,
  // conversions, 4x exports): when another format of the same range takes over, only these rows are converted,
  // so rows the new owner kept from before stay as they are (DS2 clears a 640x360 / 320x180 target with a 4x depth
  // clear of the top 352 / 176 rows and a 1x color clear of the rest; converting the whole depth image put stale
  // depth in the bottom rows: the blurred strip at the bottom of the screen, through depth of field).
  uint32_t epoch_rows = 0;
  bool owned_before = false;
  // v110: rows received by conversion when it took the range, and from which image: converting them back to
  // that same image would only replace its own (still current) pixels with a lossy round trip of them.
  uint32_t inherited_rows = 0;
  uint64_t inherited_from = 0;
  void Wrote(uint32_t bottom) { epoch_rows = std::max(epoch_rows, std::min(bottom, height)); }
  bool initialized = false;  // transitioned to GENERAL
};

VkFormat ColorRtFormat(xenos::ColorRenderTargetFormat f) {
  using F = xenos::ColorRenderTargetFormat;
  switch (f) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // RC67 test: 32-bit float storage (full-rate blending, half the bandwidth) instead of 64-bit RGBA16F.
      // Loses the 2-bit alpha of 7e3; RGB 6/6/5-bit mantissa instead of 7.
      if (REXCVAR_GET(ds2_native_7e3_as_r11g11b10)) return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case F::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case F::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

struct TextureFormatInfo {
  VkFormat format;
  uint32_t bytes_per_block;  // guest bytes per block (1-16; 8/16 for compressed formats)
  uint32_t block_size;  // texels per block side (1 or 4)
  uint32_t host_swizzle;  // XE_GPU_TEXTURE_SWIZZLE_* for components missing in the host format
  // CPU-decoded compressed textures (hosts without BCn, MoltenVK on iOS has no S3TC): the guest
  // blocks are decoded to uncompressed texels on upload - the same texels the classic path's DXT
  // load shaders produce (bcn_decode.h). 0 when the host image uses the compressed format as-is.
  uint32_t decoded_texel_bytes;
  // Bytes in the upload buffer covering the 4x4-block footprint of one guest block: the guest
  // block bytes uploaded as-is, or 16 decoded texels.
  uint32_t host_block_bytes() const { return decoded_texel_bytes ? decoded_texel_bytes * 16u : bytes_per_block; }
};

bool GetTextureFormat(xenos::TextureFormat f, bool host_bcn, TextureFormatInfo& out) {
  using T = xenos::TextureFormat;
  switch (f) {
    case T::k_8:
    case T::k_8_A:
      out = {VK_FORMAT_R8_UNORM, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case T::k_8_8:
      out = {VK_FORMAT_R8G8_UNORM, 2, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case T::k_8_8_8_8:
    case T::k_8_8_8_8_AS_16_16_16_16:
      out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case T::k_2_10_10_10:
    case T::k_2_10_10_10_AS_16_16_16_16:
      out = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case T::k_16_16_16_16_FLOAT:
      out = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case T::k_16_16_FLOAT:
      out = {VK_FORMAT_R16G16_SFLOAT, 4, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case T::k_16_FLOAT:
      out = {VK_FORMAT_R16_SFLOAT, 2, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case T::k_32_FLOAT:
      out = {VK_FORMAT_R32_SFLOAT, 4, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case T::k_16_16:
      out = {VK_FORMAT_R16G16_UNORM, 4, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case T::k_16_16_16_16:
      out = {VK_FORMAT_R16G16B16A16_UNORM, 8, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case T::k_DXT1:
      if (!host_bcn) {
        out = {VK_FORMAT_R8G8B8A8_UNORM, 8, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, 4};
        return true;
      }
      out = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case T::k_DXT2_3:
      if (!host_bcn) {
        out = {VK_FORMAT_R8G8B8A8_UNORM, 16, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, 4};
        return true;
      }
      out = {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case T::k_DXT4_5:
      if (!host_bcn) {
        out = {VK_FORMAT_R8G8B8A8_UNORM, 16, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, 4};
        return true;
      }
      out = {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case T::k_DXN:
      if (!host_bcn) {
        out = {VK_FORMAT_R8G8_UNORM, 16, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, 2};
        return true;
      }
      out = {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case T::k_DXT5A:
      if (!host_bcn) {
        out = {VK_FORMAT_R8_UNORM, 8, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, 1};
        return true;
      }
      out = {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    default:
      return false;
  }
}

// Endian swap of one element (xenos::Endian), in place.
void SwapElements(uint8_t* data, size_t bytes, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16: {
      size_t i = 0;
      for (; i + 8 <= bytes; i += 8) {
        uint64_t v;
        std::memcpy(&v, data + i, 8);
        v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
        std::memcpy(data + i, &v, 8);
      }
      for (; i + 1 < bytes; i += 2) std::swap(data[i], data[i + 1]);
    } break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = rex::byte_swap(v);
        std::memcpy(data + i, &v, 4);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(data + i, &v, 4);
      }
      break;
    default:
      break;
  }
}

// CPU-decodes a row-major rectangle of guest compressed blocks (already untiled and endian-swapped
// - the same input the direct compressed upload would give to vkCmdCopyBufferToImage) into rows
// padded to whole 4-texel blocks of uncompressed host texels (the VkBufferImageCopy extent crops
// the padding). Returns false if the format has no CPU decoder (the destination is zeroed).
bool DecodeBcnBlocks(xenos::TextureFormat format, uint32_t bytes_per_block, uint32_t decoded_texel_bytes,
                     const uint8_t* src_blocks, uint32_t lbw, uint32_t lbh, uint8_t* dst) {
  const size_t dst_row_bytes = size_t(lbw) * 4u * decoded_texel_bytes;
  const size_t src_row_bytes = size_t(lbw) * bytes_per_block;
  for (uint32_t by = 0; by < lbh; ++by) {
    uint8_t* const dst_row = dst + size_t(by) * dst_row_bytes;
    const uint8_t* const src_row = src_blocks + size_t(by) * src_row_bytes;
    for (uint32_t bx = 0; bx < lbw; ++bx) {
      if (!native::BcnDecodeTextureBlock(format, src_row + size_t(bx) * bytes_per_block,
                                         dst_row + size_t(bx) * 4u * decoded_texel_bytes, dst_row_bytes)) {
        std::memset(dst, 0, size_t(lbh) * dst_row_bytes);
        return false;
      }
    }
  }
  return true;
}

VkComponentSwizzle SwizzleComponent(uint32_t guest_swizzle, uint32_t host_swizzle, uint32_t c) {
  uint32_t s = (guest_swizzle >> (3 * c)) & 7;
  if (s == 4) return VK_COMPONENT_SWIZZLE_ZERO;
  if (s >= 5) return VK_COMPONENT_SWIZZLE_ONE;
  uint32_t h = (host_swizzle >> (3 * s)) & 7;
  switch (h) {
    case 0:
      return VK_COMPONENT_SWIZZLE_R;
    case 1:
      return VK_COMPONENT_SWIZZLE_G;
    case 2:
      return VK_COMPONENT_SWIZZLE_B;
    case 3:
      return VK_COMPONENT_SWIZZLE_A;
    case 4:
      return VK_COMPONENT_SWIZZLE_ZERO;
    default:
      return VK_COMPONENT_SWIZZLE_ONE;
  }
}

constexpr VkBlendFactor kBlendFactors[32] = {
    VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_SRC_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
};
constexpr VkBlendOp kBlendOps[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                    VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD,
                                    VK_BLEND_OP_ADD};

// Everything that selects a VkPipeline. No padding: hashed as bytes.
struct PipelineKey {
  uint64_t vs_hash;
  uint64_t vs_modification;
  uint64_t ps_hash;
  uint64_t ps_modification;
  uint64_t render_pass;  // VkRenderPass handle value
  uint32_t topology;
  uint32_t primitive_restart;
  uint32_t cull_mode;
  uint32_t front_face_cw;
  uint32_t depth_test, depth_write, depth_func;
  uint32_t stencil_enable;
  uint32_t stencil_front;  // fail | pass << 3 | zfail << 6 | func << 9
  uint32_t stencil_back;
  uint32_t blend[4];       // RB_BLENDCONTROL & 0x1FFF1FFF, or 0x00010001 for off
  uint32_t write_mask;     // 4 bits per RT
  uint32_t color_count;
  uint32_t has_ps;
  uint32_t depth_clamp;
  uint32_t samples;           // RC71: 4 for host 4x MSAA passes, else 0 (1x)
  uint32_t samples_reserved;  // keeps the key free of padding
  uint32_t texture_layout;  // texture/sampler counts for the pipeline layout
  uint32_t native_binding_count, native_attribute_count;
  uint32_t texture_signs[8];  // RC96: specialization constants with ds2_native_fold_texture_signs, else 0
  VkVertexInputBindingDescription native_bindings[8];
  VkVertexInputAttributeDescription native_attributes[32];
};
static_assert(sizeof(PipelineKey) % 8 == 0, "PipelineKey must have no tail padding");

class NativeGraphicsSystem;

class NativeCommandProcessor final : public CommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state)
      : CommandProcessor(graphics_system, kernel_state) {}
  ~NativeCommandProcessor() override = default;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address, const uint32_t* host_address,
                     uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;
  // RC81: plain register writes (all but scratch, coherency and gamma ramp registers) only store the value; the
  // base class looked up register info and walked a switch for every one (~15 % of the command thread).
  static bool IsSpecialRegister(uint32_t index) {
    return (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) ||
           index == XE_GPU_REG_COHER_STATUS_HOST || index == XE_GPU_REG_DC_LUT_RW_INDEX ||
           index == XE_GPU_REG_DC_LUT_SEQ_COLOR || index == XE_GPU_REG_DC_LUT_PWL_DATA ||
           index == XE_GPU_REG_DC_LUT_30_COLOR;
  }
  void WriteRegister(uint32_t index, uint32_t value) override {
    if (fast_registers_ && index < RegisterFile::kRegisterCount && !IsSpecialRegister(index)) {
      const_cast<volatile uint32_t&>(register_file_->values[index]) = value;
      return;
    }
    CommandProcessor::WriteRegister(index, value);
  }
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override {
    // Entirely above the gamma ramp registers (all constants) or below the scratch registers: no side effects.
    if (fast_registers_ && uint64_t(start_index) + num_registers <= RegisterFile::kRegisterCount &&
        (start_index > XE_GPU_REG_DC_LUT_30_COLOR || uint64_t(start_index) + num_registers <= XE_GPU_REG_SCRATCH_REG0)) {
      volatile uint32_t* dst = register_file_->values + start_index;
      for (uint32_t i = 0; i < num_registers; ++i) dst[i] = memory::load_and_swap<uint32_t>(base + i);
      return;
    }
    CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
  }
  bool fast_registers_ = true;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet, uint32_t count) override;

 private:
  struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    VkBuffer upload = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    uint32_t upload_memory_type = 0;
    VkDeviceSize upload_memory_size = 0;
    uint8_t* upload_mapping = nullptr;
    VkDeviceSize upload_used = 0;
    UploadContentCache content_cache;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet shared_memory_set = VK_NULL_HANDLE;
    std::map<std::vector<uint64_t>, VkDescriptorSet> texture_sets;
    std::map<std::array<VkDeviceSize, SpirvShaderTranslator::kConstantBufferCount>, VkDescriptorSet> constant_sets;
    std::vector<std::pair<VkImage, VkDeviceMemory>> deferred_destroy;
    std::vector<VkImageView> deferred_views;
    std::vector<VkFramebuffer> deferred_framebuffers;
    // GPU profile: timestamp i starts segment i (label query_labels[i], draws seg_draws[i]).
    VkQueryPool query_pool = VK_NULL_HANDLE;
    uint32_t queries = 0;
    std::vector<uint16_t> query_labels;
    std::vector<uint32_t> seg_draws;
    // Occlusion queries recorded in this buffer: (guest sample count address, query index).
    VkQueryPool occlusion_pool = VK_NULL_HANDLE;
    uint32_t occlusion_used = 0;
    std::vector<std::pair<uint32_t, uint32_t>> occlusion_pending;
  };
  static constexpr uint32_t kMaxOcclusionQueries = 512;
  static constexpr uint32_t kMaxQueries = 2048;
  struct PipelineLayoutEntry {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_pixel = VK_NULL_HANDLE;
  };
  struct RenderPassEntry {
    VkRenderPass pass = VK_NULL_HANDLE;
  };
  struct TextureEntry {
    NativeImage image;
    uint64_t content_hash = 0;
    uint64_t last_check_frame = 0;
    uint64_t last_use_frame = 0;
    uint64_t dynamic_until = 0;  // RC86: content changed recently (videos): checked every frame until this frame
    uint64_t resolve_signature = 0;  // cube faces copied from resolve targets: their generations
    std::unordered_map<uint32_t, VkImageView> swizzle_views;
  };
  struct ResolveLogicalImage {
    NativeImage image;
    uint64_t generation = UINT64_MAX;
    std::unordered_map<uint32_t, VkImageView> swizzle_views;
  };
  struct ResolvedEntry {
    NativeImage image;
    uint32_t guest_format = 0;
    bool rb_swapped = false;  // RB_COPY_DEST_INFO.copy_dest_swap: red and blue swapped in the guest copy
    uint32_t guest_pitch = 0, guest_height = 0, bpp_log2 = 2;  // guest texture layout (tiled), for sub-resolves
    uint32_t sample_scale_x = 1, sample_scale_y = 1;
    float scale = 1.0f;  // v107: resolved from a render target at the reduced internal resolution
    uint64_t generation = 0;
    uint64_t frame = 0;
    // v106: the last plain copy into this entry, to skip a resolve that would copy the same unchanged pixels.
    VkImage last_src = VK_NULL_HANDLE;
    uint64_t last_src_write = 0, last_dst_write = 0, last_generation = UINT64_MAX;
    int32_t last_rect[6] = {};
    uint32_t last_aspect = 0;
    std::unordered_map<uint32_t, VkImageView> swizzle_views;
    // Guest fetch dimensions may be smaller than the tiled pitch or 8-row resolve rectangle.
    // Keep the root image unchanged: sub-resolves still write into the complete atlas.
    std::unordered_map<uint64_t, ResolveLogicalImage> logical_images;
    // RC64: R32F depth decoded from an RGBA8 depth resolve, per (width, height, float24).
    std::unordered_map<uint64_t, ResolveLogicalImage> depth_decoded;
  };
  struct Stats {
    uint64_t draws = 0, draws_skipped = 0, draws_unsupported_prim = 0, draws_failed = 0;
    uint64_t copies = 0, clears = 0, swaps = 0, swaps_presented = 0;
    uint64_t texture_uploads = 0, texture_unsupported = 0, texture_from_resolve = 0;
    uint64_t texture_1d = 0, texture_white_dimension = 0, texture_cube = 0, cube_faces_from_resolves = 0;
    uint64_t texture_3d = 0;  // v104
    uint64_t resolves_skipped = 0;  // v106
    uint64_t depth_decoded_fetches = 0, depth_decodes = 0, depth_float24_quantized = 0;
    uint64_t barriers_issued = 0, barriers_elided = 0;
    uint64_t state_calls = 0, state_calls_skipped = 0;
    uint64_t face_clears = 0;
    uint64_t msaa_draws = 0, msaa_imports = 0, msaa_imports_skipped = 0, msaa_exports = 0;
    uint64_t upload_bytes = 0, pipelines = 0, render_passes_begun = 0;
    uint64_t prim_counts[32] = {};
    uint64_t conversions = 0, texture_levels = 0, texture_upload_bytes = 0, submits = 0;
    uint64_t early_z_draws = 0, draws_pipeline_pending = 0, pipelines_async = 0, pipelines_stolen = 0;
    uint64_t skip_no_vs = 0, skip_no_raster = 0, skip_no_pitch = 0, skip_no_target = 0;
    uint64_t occlusion_begins = 0, occlusion_ends = 0, occlusion_lost = 0, occlusion_visible = 0;
    // CPU time on the command processor thread, microseconds.
    uint64_t us_fence_wait = 0, us_draws = 0, us_textures = 0, us_pipelines = 0, us_copies = 0, us_frame = 0;
    uint64_t us_constants = 0, us_descriptors = 0, us_geometry = 0, us_commands = 0;
    uint64_t texture_sets_reused = 0, texture_sets_created = 0, texture_sets_kept = 0, texture_sets_dropped = 0;
    uint64_t constant_uploads_reused = 0, us_prelude = 0, us_translate = 0, us_pipeline_sync = 0, pipelines_sync = 0;
    uint64_t shader_restores_async = 0, draws_shader_pending = 0, halfres_draws = 0, halfres_composites = 0;
    uint64_t dynamic_texture_updates = 0, halfres_depth_downsamples = 0;
    uint64_t constant_sets_reused = 0, constant_sets_created = 0, resolve_image_barriers = 0;
    uint64_t native_vertex_draws = 0, legacy_vertex_draws = 0;
    uint64_t content_hits = 0, content_saved_bytes = 0;
  };
  struct ProfileLabel {
    std::string name;
    double ms = 0;
    uint64_t count = 0, draws = 0;
  };

  const VulkanDevice* device() const { return vulkan_device_; }
  const VulkanDevice::Functions& dfn() const { return vulkan_device_->functions(); }

  bool BeginFrame();
  void SubmitFrame();
  FrameSlot& slot() { return slots_[frame_ % kFrameSlots]; }
  uint8_t* Upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset_out);
  bool UploadImmutable(const void* data, size_t size, VkDeviceSize alignment, uint32_t tag, VkDeviceSize& offset);
  void Barrier();
  void ResolveBarrier(NativeImage& src, NativeImage& dst, bool before);
  void EndRenderPass();
  bool CreateImage(NativeImage& img, VkFormat format, uint32_t w, uint32_t h, bool depth, VkImageUsageFlags usage,
                   uint32_t levels = 1, bool cube = false, uint32_t samples = 1, uint32_t volume_depth = 0);
  void DestroyImage(NativeImage& img, bool deferred);
  void EnsureGeneral(NativeImage& img);
  NativeImage* GetRenderTarget(uint32_t base, uint32_t pitch, uint32_t format, bool depth, uint32_t height_hint);
  VkRenderPass GetRenderPass(const VkFormat* color_formats, uint32_t color_count, VkFormat depth_format,
                             uint32_t samples = 1);
  // RC71: host 4x MSAA. A 4x render target keeps its sample-space image (the EDRAM view every other pass, resolve
  // and conversion uses) and gets a 4x image of half the size for the passes drawn in 4x MSAA: pixel shaders run
  // once per pixel there, as on the Xbox 360, instead of once per sample. Contents move between the two only
  // when the other one is needed.
  struct MsaaShadow {
    NativeImage image;
    bool valid = false;  // holds the current contents (imported, or drawn after a full clear)
    bool newer = false;  // drawn since the last export to the sample-space image
    uint32_t dirty_rows = 0;  // RC72: rows (of the 4x image) drawn since the last export
  };
  // RC72: 4x passes with real geometry (not clear rectangles) per color target, this and the previous frame.
  // Only targets with enough of them use the 4x copies (DS2 also clears with 4x rectangles and then draws at 1x:
  // a 4x copy there costs an export every frame and saves nothing).
  std::unordered_map<VkImage, uint32_t> msaa_heavy_this_frame_, msaa_heavy_last_frame_;
  std::unordered_map<VkImage, MsaaShadow> msaa_shadows_;
  bool msaa_supported_ = false;
  // The device can sample BCn (S3TC) images; false = guest compressed textures are CPU-decoded.
  bool host_bcn_ = true;
  bool msaa_lookup_ = false;  // GetRenderTarget calls of a 4x pass: no export
  bool msaa_force_clear_ = false;   // RC73: replaying a 4x clear rectangle into the 4x copies
  bool msaa_replay_clear_ = false;  // RC73: the current clear rectangle is to be replayed into the 4x copies
  std::unordered_map<uint64_t, VkPipeline> msaa_pipelines_;
  VkShaderModule msaa_fs_[8] = {};
  NativeImage* GetMsaaShadow(NativeImage& rt, bool import, uint32_t import_rows = 0);
  void ExportMsaaShadow(NativeImage& rt, bool invalidate);
  void ExportAllMsaaShadows();
  bool MsaaTransfer(NativeImage& rt, NativeImage& ms, bool to_ms, uint32_t rows = 0);
  bool QuantizeDepthFloat24(NativeImage& src, NativeImage& dst, int32_t src_x, int32_t src_y, const VkRect2D& dst_rect);
  VkPipeline GetMsaaPipeline(uint32_t mode, VkFormat format, bool ms, VkRenderPass pass);
  const PipelineLayoutEntry* GetPipelineLayout(uint32_t tv, uint32_t sv, uint32_t tp, uint32_t sp);
  VkDescriptorSetLayout GetTextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  VkPipeline GetPipeline(const PipelineKey& key, VulkanShader::VulkanTranslation* vs,
                         VulkanShader::VulkanTranslation* ps, VkPipelineLayout layout, bool allow_async,
                         bool& pending_out);
  VkPipeline CreatePipelineObject(const PipelineKey& key, VkShaderModule vs, VkShaderModule ps,
                                  VkPipelineLayout layout);
  static void* PipelineThreadMain(void* self);
  void StopPipelineThread();
  VkSampler GetSampler(const VulkanShader::SamplerBinding& binding, const xenos::xe_gpu_texture_fetch_t& fetch);
  VkImageView GetTextureView(uint32_t fetch_index, xenos::FetchOpDimension dimension, bool& resolved_out);
  VkImageView GetSwizzleView(std::unordered_map<uint32_t, VkImageView>& views, const NativeImage& image,
                             uint32_t swizzle);
  ResolveLogicalImage* GetResolveLogicalImage(ResolvedEntry& entry, uint32_t width, uint32_t height);
  void DestroyResolveLogicalImages(ResolvedEntry& entry, bool deferred);
  bool ClearRect(NativeImage& rt, const VkClearValue& value, const VkRect2D& rect, bool depth_only = false);
  // RC74: a depth-only clear rectangle followed by depth-only geometry in a smaller viewport (point light shadow
  // cube faces packed in one target): the face is cleared to the clear depth over its whole viewport.
  struct PendingFaceClear {
    VkImage image = VK_NULL_HANDLE;
    float depth = 0.0f;
  } pending_face_clear_;
  bool SetupConversions();
  bool Convert(ConvertMode mode, NativeImage& src, NativeImage& dst, int32_t src_x, int32_t src_y,
               const VkRect2D& dst_rect, bool src_float24, int32_t resolve_exp_bias = 0);
  NativeImage* PrepareColorResolve(NativeImage& src, const VkRect2D& rect, int32_t exp_bias);
  void AdoptRenderTarget(uint32_t base, uint32_t pitch, uint64_t key, NativeImage& img);
  void LogStats();
  void LogGammaRamp();
  uint64_t last_gamma_ramp_hash_ = 0;
  bool work_since_barrier_ = true;  // RC66: GPU work recorded since the last pipeline barrier
  // RC68: the draw state last recorded in the command buffer, to skip redundant vkCmd* calls (NVK re-emits the
  // state of every bound pipeline and dynamic state at the next draw).
  struct DrawStateCache {
    bool valid = false;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet sets[4] = {};
    uint32_t offset_count = 0;
    uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount] = {};
    VkViewport viewport = {};
    VkRect2D scissor = {};
    float bias[2] = {};
    float blend[4] = {};
    uint32_t stencil[6] = {};
  } draw_state_;
  void InvalidateDrawState() { draw_state_.valid = false; }
  void DumpImages();
  void DumpImage(const std::string& name, NativeImage& image, bool alpha);
  void EvictTextures();
  // GPU profile: a timestamp that ends the current segment and starts one labelled `label`.
  void Mark(const std::string& label);
  void MarkPass(const VkFormat* color_formats, uint32_t color_count, VkFormat depth_format, uint32_t w, uint32_t h);
  void CollectProfile(FrameSlot& s);
  uint32_t ReadPixel(NativeImage& img, uint32_t x, uint32_t y, VkImageAspectFlags aspect);
  bool Tracing() const {
    return combo_tracing_ || (REXCVAR_GET(ds2_native_dump_swap) > 0 && !dumped_ &&
                              heavy_frames_ + 1 == uint64_t(REXCVAR_GET(ds2_native_dump_swap)));
  }
  std::string DumpDir() const;
  void PollDumpCombo();
  bool combo_tracing_ = false;   // this frame is traced because the player asked for a capture
  uint32_t combo_held_frames_ = 0;
  uint32_t combo_dumps_ = 0;

  const VulkanDevice* vulkan_device_ = nullptr;
  PFN_vkCmdBlitImage cmd_blit_image_ = nullptr;
  PFN_vkCmdCopyImage cmd_copy_image_ = nullptr;
  PFN_vkCmdSetDepthBias2EXT cmd_set_depth_bias2_ = nullptr;
  PFN_vkFreeDescriptorSets free_descriptor_sets_ = nullptr;  // RC76: absolute (unorm24) constant depth bias
  std::unique_ptr<SpirvShaderTranslator> translator_;
  // RC79: shader pack restores (SD reads, ~10 ms each) on a worker. A draw whose shader is still being restored is
  // skipped (as with a pipeline still compiling); rectangles and full-screen passes still restore at once.
  std::mutex restore_mutex_;
  std::condition_variable restore_cv_;
  std::deque<Shader::Translation*> restore_jobs_;
  std::vector<std::pair<Shader::Translation*, bool>> restore_done_;
  std::unordered_set<Shader::Translation*> restore_pending_;
  std::unordered_map<const Shader*, uint32_t> restore_pending_shaders_;
  std::unordered_set<Shader::Translation*> restore_failed_;
  pthread_t restore_thread_ = {};
  bool restore_thread_started_ = false, restore_quit_ = false;
  uint64_t restore_signature_ = 0;
  std::atomic<uint64_t> restore_us_{0}, restore_count_{0};
  static void* RestoreThreadMain(void* param);
  void DrainRestores(bool wait_for_shader, const Shader* shader);
  void StopRestoreThread();
  // Returns true when the translation is ready to use now; false when it is being restored (the draw waits).
  bool PrepareTranslation(VulkanShader::VulkanTranslation* t, bool allow_async);
  string::StringBuffer ucode_disasm_buffer_;
  std::unordered_map<uint64_t, std::unique_ptr<VulkanShader>> shaders_;

  std::array<FrameSlot, kFrameSlots> slots_;
  uint64_t frame_ = 0;
  bool frame_open_ = false;
  VkPipeline profile_shader_pipeline_ = VK_NULL_HANDLE;
  uint32_t profile_shader_groups_ = 0;
  std::array<uint32_t, 3> profile_shader_phase_groups_ = {};
  bool profile_shader_active_ = false;

  VkDescriptorSetLayout set_layout_shared_memory_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_constants_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_empty_ = VK_NULL_HANDLE;
  uint32_t shared_memory_bindings_ = 1;
  std::unordered_map<uint32_t, VkDescriptorSetLayout> texture_set_layouts_;
  std::unordered_map<uint64_t, PipelineLayoutEntry> pipeline_layouts_;
  std::unordered_map<uint64_t, VkRenderPass> render_passes_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
  std::unordered_multimap<VkImageView, uint64_t> fb_by_view_;  // to drop framebuffers of destroyed images
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  std::unordered_map<uint64_t, VkSampler> samplers_;
  std::unordered_map<uint64_t, NativeImage> render_targets_;
  std::unordered_map<uint64_t, TextureEntry> textures_;
  std::unordered_map<uint32_t, ResolvedEntry> resolved_;
  NativeImage white_;
  NativeImage white_cube_;  // RC63+: cube bindings need a cube view (a 2D view there is undefined on NVK)
  NativeImage white_3d_;    // v104: same for 3D bindings
  VkImageView GetCubeTextureView(const xenos::xe_gpu_texture_fetch_t& fetch);
  VkImageView Get3DTextureView(const xenos::xe_gpu_texture_fetch_t& fetch);
  VkImageView GetResolveDepthView(ResolvedEntry& entry, uint32_t width, uint32_t height, bool float24,
                                  uint32_t guest_swizzle);
  // Format reinterpretation passes.
  VkShaderModule convert_vs_ = VK_NULL_HANDLE;
  VkShaderModule convert_fs_[kConvertModeCount] = {};
  VkDescriptorSetLayout convert_set_layout_[2] = {};  // 1 and 2 combined samplers
  VkPipelineLayout convert_layout_[2] = {};
  VkSampler convert_sampler_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkPipeline> convert_pipelines_;
  NativeImage color_resolve_scratch_;  // converted source pixels, never the root atlas
  VkShaderModule present_fs_ = VK_NULL_HANDLE;
  VkShaderModule probe_fs_ = VK_NULL_HANDLE;  // RC91: constant pixel shader of ds2_native_probe = 3
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  VkRenderPass present_pass_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkFramebuffer> present_framebuffers_;
  // Last image written at an EDRAM base and sample pitch: (base | pitch << 12) -> render target key.
  std::unordered_map<uint64_t, uint64_t> edram_owner_;
  uint64_t conversions_ = 0;
  VkImageView white_view_ = VK_NULL_HANDLE;

  // Current render pass.
  bool in_render_pass_ = false;
  uint64_t current_framebuffer_key_ = 0;
  VkRenderPass current_render_pass_ = VK_NULL_HANDLE;
  uint32_t current_rt_width_ = 0, current_rt_height_ = 0;

  // Per-frame vertex buffer dedupe: (guest address << 32 | size) -> upload offset.
  std::unordered_map<uint64_t, VkDeviceSize> frame_vertex_uploads_;
  std::unordered_map<uint64_t, NativeVertexLayout> native_vertex_layouts_;
  std::vector<uint32_t> native_indices_;
  std::vector<VkWriteDescriptorSet> descriptor_writes_;
  std::vector<uint64_t> descriptor_key_;
  std::vector<uint8_t> texture_row_scratch_;
  // Guest compressed blocks staged here before CPU decoding when the host has no BCn.
  std::vector<uint8_t> texture_block_scratch_;
  std::vector<VkDescriptorImageInfo> draw_image_infos_;
  // RC82: additive particle draws listed in ds2_native_halfres_ps are drawn into a half-resolution copy and added to
  // the target (bonfire fog: one draw of 16 full-screen quads took 47-64 ms of GPU time).
  struct HalfResTarget {
    NativeImage image;
  };
  std::unordered_map<VkImage, HalfResTarget> halfres_targets_;
  uint64_t write_counter_ = 0;  // v106: source of NativeImage::write_id
  uint64_t cap_next_us_ = 0;     // v106: ds2_fps_cap, when the next frame may be presented
  bool swap_interval_set_ = false;  // v116
  // v107: reduced internal resolution. Render targets 1280 samples wide (the DS2 scene: 1280x720 1x and 640x360
  // 4x) are created at res_scale_ of their size; draws into them scale viewport and scissor, and their pixel
  // shaders see guest pixel positions (gl_FragCoord patch, see sdk/src/graphics/vulkan/shader.cpp).
  float res_scale_ = 1.0f;
  bool res_scale_latched_ = false;
  void LatchRenderScale();
  bool IsScaledPitch(uint32_t sample_pitch) const { return res_scale_ < 1.0f && sample_pitch == 1280; }
  uint32_t Sc(uint32_t v) const { return uint32_t(std::lround(double(v) * double(res_scale_))); }
  int32_t Sc(int32_t v) const { return int32_t(std::lround(double(v) * double(res_scale_))); }
  void ScaleScissor(draw_util::Scissor& sc) const {
    const uint32_t x0 = Sc(sc.offset[0]), y0 = Sc(sc.offset[1]);
    const uint32_t x1 = Sc(sc.offset[0] + sc.extent[0]), y1 = Sc(sc.offset[1] + sc.extent[1]);
    sc.offset[0] = x0;
    sc.offset[1] = y0;
    sc.extent[0] = x1 - x0;
    sc.extent[1] = y1 - y0;
  }
  void Written(NativeImage& img) { img.write_id = ++write_counter_; }
  VkPipeline halfres_composite_pipeline_ = VK_NULL_HANDLE;
  VkPipeline halfres_composite_pipeline_msaa_ = VK_NULL_HANDLE;  // v105: into a host 4x MSAA copy
  VkShaderModule halfres_composite_fs_ = VK_NULL_HANDLE;
  VkSampler linear_sampler_ = VK_NULL_HANDLE;
  bool halfres_draw_ = false;
  uint32_t halfres_divisor_ = 2;
  // RC84: consecutive reduced-resolution draws accumulate in one copy (color, transmittance in alpha) that is
  // composited once when another draw, a resolve or the swap comes.
  NativeImage* halfres_pending_rt_ = nullptr;
  NativeImage* halfres_pending_half_ = nullptr;
  VkRect2D halfres_pending_rect_ = {};
  void FlushHalfRes();
  bool IsHalfResPixelShader(uint64_t hash);
  uint32_t HalfResMode(uint64_t hash);
  bool DownsampleDepth(NativeImage& src, NativeImage& dst, uint32_t divisor, bool use_max);
  std::unordered_map<VkImage, HalfResTarget> halfres_depth_targets_;
  NativeImage* halfres_pending_depth_src_ = nullptr;
  uint32_t halfres_mode_ = 0;
  bool CompositeHalfRes(NativeImage& rt, NativeImage& half, const VkRect2D& rect);
  struct LastConstantUpload {
    std::vector<uint8_t> data;
    VkDeviceSize offset = 0;
    uint64_t frame = UINT64_MAX;
  };
  LastConstantUpload last_constant_uploads_[SpirvShaderTranslator::kConstantBufferCount];
  // RC77: texture descriptor sets kept across frames (keyed by layout, views, samplers), dropped when a view they
  // use is destroyed. Per-frame sets (slot().texture_sets) remain the fallback when the pools are full.
  struct KeyVectorHash {
    size_t operator()(const std::vector<uint64_t>& k) const {
      return size_t(XXH3_64bits(k.data(), k.size() * sizeof(uint64_t)));
    }
  };
  struct PersistentSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
  };
  std::unordered_map<std::vector<uint64_t>, PersistentSet, KeyVectorHash> persistent_texture_sets_;
  std::unordered_map<VkImageView, std::vector<std::vector<uint64_t>>> persistent_sets_by_view_;
  std::vector<VkDescriptorPool> persistent_pools_;
  bool AllocatePersistentSet(VkDescriptorSetLayout layout, PersistentSet& out);
  void DestroyView(VkImageView view);

  SpirvShaderTranslator::SystemConstants system_constants_ = {};
  Stats stats_;
  uint64_t last_log_frame_ = 0;
  uint32_t probe_active_ = 0;     // RC90: ds2_native_probe mode applied in this log interval (0 = not now)
  uint64_t probe_intervals_ = 0;  // RC90: log intervals since start
  uint64_t draws_this_frame_ = 0, heavy_frames_ = 0;
  bool dumped_ = false;

  // Async pipeline compilation (v55).
  struct PipelineJob {
    uint64_t hash;
    PipelineKey key;
    VkShaderModule vs, ps;
    VkPipelineLayout layout;
  };
  std::mutex pipeline_mutex_;
  std::condition_variable pipeline_cv_;
  std::deque<PipelineJob> pipeline_jobs_;
  std::vector<std::pair<uint64_t, VkPipeline>> pipeline_done_;
  std::atomic<uint32_t> pipeline_done_count_{0};
  std::unordered_set<uint64_t> pipeline_pending_;
  pthread_t pipeline_thread_{};
  bool pipeline_thread_started_ = false;
  bool pipeline_quit_ = false;
  std::atomic<uint64_t> pipeline_async_us_{0};
  // Hitch report: the counters at the previous swap.
  uint64_t hitch_prev_[6] = {};
  uint64_t hitch_prev_pipelines_ = 0;
  uint64_t hitch_prev_extra_[4] = {};

  // Occlusion queries: last measured sample count per guest result address; the active query index or -1.
  struct OcclusionResult {
    uint32_t samples = 0;      // latest measurement
    uint32_t max_recent = 0;   // largest measurement since the last zero streak began
    uint32_t zero_streak = 0;  // consecutive zero measurements
  };
  std::unordered_map<uint32_t, OcclusionResult> occlusion_results_;
  void NoteOcclusionResult(uint32_t address, uint32_t samples) {
    OcclusionResult& r = occlusion_results_[address];
    r.samples = samples;
    if (samples) {
      r.zero_streak = 0;
      r.max_recent = samples;
    } else {
      ++r.zero_streak;
    }
  }
  int32_t occlusion_active_ = -1;
  bool occlusion_unmeasured_ = false;
  void EndOcclusionQuery(uint32_t result_address);

  // GPU profile.
  bool profiling_ = false;       // timestamps available and requested
  // RC80: after a frame that waited long for the GPU, every command buffer of the next frames is timed with
  // per-shader groups, and the slow ones are logged on their own (not averaged away over 10 s).
  uint32_t spike_frames_ = 0, spike_logs_ = 0;
  uint64_t spike_last_log_us_ = 0;
  bool profile_capable_ = false;  // RC66: profiling_ is set per command buffer (1 in ds2_native_gpu_profile_every)
  uint64_t profile_counter_ = 0;
  double timestamp_period_ns_ = 1.0;
  uint64_t timestamp_mask_ = ~uint64_t(0);
  std::vector<ProfileLabel> profile_labels_;
  std::unordered_map<std::string, uint16_t> profile_label_ids_;
  double profile_gpu_ms_ = 0;      // sum of command buffer spans
  uint64_t profile_buffers_ = 0;   // command buffers measured
  uint64_t last_swap_us_ = 0;
};

uint64_t NowUs() {
#if defined(__aarch64__) && REX_PLATFORM_SWITCH
  // RC87: the system counter (19.2 MHz on the Switch) instead of steady_clock (~10 timer reads per draw).
  uint64_t ticks;
  asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));  // cntvct_el0 traps on Horizon (RC87 crash)
  return ticks * 10 / 192;
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}

// Adds the time of a scope to a counter.
struct ScopedUs {
  uint64_t& counter;
  uint64_t start;
  explicit ScopedUs(uint64_t& c) : counter(c), start(NowUs()) {}
  ~ScopedUs() { counter += NowUs() - start; }
};

// ---------------------------------------------------------------------------------------------------------------

bool NativeCommandProcessor::SetupContext() {
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(graphics_system_->provider());
  if (!provider) {
    REXGPU_ERROR("[native] no Vulkan provider");
    return false;
  }
  vulkan_device_ = provider->vulkan_device();
  fast_registers_ = REXCVAR_GET(ds2_native_fast_registers);
  const auto& props = vulkan_device_->properties();
  const VkDevice dev = vulkan_device_->device();
  const auto& ifn = vulkan_device_->vulkan_instance()->functions();
  cmd_blit_image_ = reinterpret_cast<PFN_vkCmdBlitImage>(ifn.vkGetDeviceProcAddr(dev, "vkCmdBlitImage"));
  cmd_copy_image_ = reinterpret_cast<PFN_vkCmdCopyImage>(ifn.vkGetDeviceProcAddr(dev, "vkCmdCopyImage"));
  if (!cmd_blit_image_ || !cmd_copy_image_) {
    REXGPU_ERROR("[native] vkCmdBlitImage / vkCmdCopyImage not available");
    return false;
  }
  free_descriptor_sets_ =
      reinterpret_cast<PFN_vkFreeDescriptorSets>(ifn.vkGetDeviceProcAddr(dev, "vkFreeDescriptorSets"));
  if (!free_descriptor_sets_) REXCVAR_SET(ds2_native_persistent_texture_sets, false);
  if (vulkan_device_->extensions().ext_EXT_depth_bias_control && props.depthBiasControl &&
      props.leastRepresentableValueForceUnormRepresentation) {
    cmd_set_depth_bias2_ =
        reinterpret_cast<PFN_vkCmdSetDepthBias2EXT>(ifn.vkGetDeviceProcAddr(dev, "vkCmdSetDepthBias2EXT"));
  }
  REXGPU_INFO("[native] absolute depth bias (VK_EXT_depth_bias_control, unorm24 units): {}",
              cmd_set_depth_bias2_ && REXCVAR_GET(ds2_native_depth_bias_absolute) ? "on" : "off");

  // BCn (S3TC) support: on hosts that can't sample compressed textures (MoltenVK on iOS has no
  // S3TC) the guest DXT blocks are CPU-decoded to uncompressed texels on upload (bcn_decode.h,
  // the same texels the classic path's DXT load shaders produce). Same feature check as the
  // classic texture cache (linearly filterable sampled images, optimal tiling).
  host_bcn_ = true;
  if (REXCVAR_GET(ds2_native_bcn_decode)) {
    constexpr VkFormatFeatureFlags bcn_features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const VkFormat bcn_formats[5] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
                                     VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK,
                                     VK_FORMAT_BC5_UNORM_BLOCK};
    for (const VkFormat bcn_format : bcn_formats) {
      VkFormatProperties bcn_properties = {};
      ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device_->physical_device(), bcn_format,
                                              &bcn_properties);
      if ((bcn_properties.optimalTilingFeatures & bcn_features) != bcn_features) {
        host_bcn_ = false;
        break;
      }
    }
  }
  REXGPU_INFO("[native] host BCn (S3TC): {} - compressed guest textures {}",
              host_bcn_ ? "yes" : "no",
              host_bcn_ ? "uploaded as-is" : "CPU-decoded on upload (ds2_native_bcn_decode)");

  translator_ = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(vulkan_device_), false,
                                                        false, false, 1, 1);
  translator_->set_fold_texture_signs(REXCVAR_GET(ds2_native_fold_texture_signs));
  REXGPU_INFO("[native] texture signs as specialization constants: {}",
              REXCVAR_GET(ds2_native_fold_texture_signs) ? "on" : "off");
  shared_memory_bindings_ =
      uint32_t(1) << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(props.maxStorageBufferRange);

  const VkShaderStageFlags guest_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                          (props.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  // Set 0: "shared memory" = the frame's upload buffer.
  {
    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b.descriptorCount = shared_memory_bindings_;
    b.stageFlags = guest_stages;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &b;
    if (dfn().vkCreateDescriptorSetLayout(dev, &ci, nullptr, &set_layout_shared_memory_) != VK_SUCCESS) {
      return false;
    }
  }
  // Set 1: the five constant buffers.
  {
    VkDescriptorSetLayoutBinding b[SpirvShaderTranslator::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      b[i].binding = i;
      b[i].descriptorType = REXCVAR_GET(ds2_native_dynamic_ubo) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      b[i].descriptorCount = 1;
      b[i].stageFlags = guest_stages;
    }
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
    ci.pBindings = b;
    if (dfn().vkCreateDescriptorSetLayout(dev, &ci, nullptr, &set_layout_constants_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetLayoutCreateInfo empty = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (dfn().vkCreateDescriptorSetLayout(dev, &empty, nullptr, &set_layout_empty_) != VK_SUCCESS) {
      return false;
    }
  }

  for (FrameSlot& s : slots_) {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
    if (dfn().vkCreateCommandPool(dev, &pci, nullptr, &s.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (dfn().vkAllocateCommandBuffers(dev, &ai, &s.cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dfn().vkCreateFence(dev, &fci, nullptr, &s.fence) != VK_SUCCESS) return false;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device_, kUploadBufferSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, s.upload, s.upload_memory, &s.upload_memory_type,
            &s.upload_memory_size)) {
      REXGPU_ERROR("[native] upload buffer creation failed");
      return false;
    }
    void* map = nullptr;
    if (dfn().vkMapMemory(dev, s.upload_memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) return false;
    s.upload_mapping = static_cast<uint8_t*>(map);
    VkDescriptorPoolSize sizes[5] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kMaxSetsPerFrame * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxSetsPerFrame * 2},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxSetsPerFrame * 4},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSetsPerFrame * 4},
    };
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = kMaxSetsPerFrame;
    dpci.poolSizeCount = 5;
    dpci.pPoolSizes = sizes;
    if (dfn().vkCreateDescriptorPool(dev, &dpci, nullptr, &s.descriptor_pool) != VK_SUCCESS) return false;
  }

  for (FrameSlot& s : slots_) {
    VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_OCCLUSION;
    qci.queryCount = kMaxOcclusionQueries;
    if (dfn().vkCreateQueryPool(dev, &qci, nullptr, &s.occlusion_pool) != VK_SUCCESS) {
      s.occlusion_pool = VK_NULL_HANDLE;
    }
  }

  if (REXCVAR_GET(ds2_native_gpu_profile)) {
    const auto& ifn2 = vulkan_device_->vulkan_instance()->functions();
    VkPhysicalDeviceProperties pdp;
    ifn2.vkGetPhysicalDeviceProperties(vulkan_device_->physical_device(), &pdp);
    uint32_t qf_count = 0;
    ifn2.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count);
    ifn2.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &qf_count, qf.data());
    const uint32_t family = vulkan_device_->queue_family_graphics_compute();
    const uint32_t bits = family < qf_count ? qf[family].timestampValidBits : 0;
    if (bits && pdp.limits.timestampPeriod > 0) {
      timestamp_period_ns_ = pdp.limits.timestampPeriod;
#if REX_PLATFORM_SWITCH
      // Measured on the console (v54): NVK reports 1 ns, but the Tegra X1 GPU timer runs at 614.4 MHz (the
      // NVN conversion is ticks * 625 / 384): with 1 ns the GPU time came out 1.63x short of the frame time.
      if (timestamp_period_ns_ == 1.0) timestamp_period_ns_ = 625.0 / 384.0;
#endif
      timestamp_mask_ = bits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1);
      profiling_ = true;
      profile_capable_ = true;
      for (FrameSlot& s : slots_) {
        VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxQueries;
        if (dfn().vkCreateQueryPool(dev, &qci, nullptr, &s.query_pool) != VK_SUCCESS) {
          profiling_ = false;
          break;
        }
        s.query_labels.reserve(kMaxQueries);
        s.seg_draws.reserve(kMaxQueries);
      }
    }
    REXGPU_INFO("[native] GPU profile {}: timestamp bits {}, period {} ns", profiling_ ? "on" : "unavailable", bits,
                pdp.limits.timestampPeriod);
  }

  if (!BeginFrame()) return false;
  // 1x1 white texture for fetches that cannot be served.
  if (!CreateImage(white_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    return false;
  }
  EnsureGeneral(white_);
  {
    VkClearColorValue c = {};
    c.float32[0] = c.float32[1] = c.float32[2] = c.float32[3] = 1.0f;
    VkImageSubresourceRange r = ui::vulkan::util::InitializeSubresourceRange();
    work_since_barrier_ = true;
    dfn().vkCmdClearColorImage(slot().cmd, white_.image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    Barrier();
  }
  white_view_ = white_.view;
  if (CreateImage(white_cube_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, true)) {
    EnsureGeneral(white_cube_);
    VkClearColorValue c = {};
    c.float32[0] = c.float32[1] = c.float32[2] = c.float32[3] = 1.0f;
    VkImageSubresourceRange r = ui::vulkan::util::InitializeSubresourceRange();
    work_since_barrier_ = true;
    dfn().vkCmdClearColorImage(slot().cmd, white_cube_.image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    Barrier();
  }
  if (CreateImage(white_3d_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, false, 1, 1)) {
    EnsureGeneral(white_3d_);
    VkClearColorValue c = {};
    c.float32[0] = c.float32[1] = c.float32[2] = c.float32[3] = 1.0f;
    VkImageSubresourceRange r = ui::vulkan::util::InitializeSubresourceRange();
    work_since_barrier_ = true;
    dfn().vkCmdClearColorImage(slot().cmd, white_3d_.image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    Barrier();
  }
  if (!SetupConversions()) {
    REXGPU_WARN("[native] format reinterpretation passes unavailable");
  }
  msaa_supported_ = vulkan_device_->extensions().ext_EXT_sample_locations && convert_vs_ &&
                    (props.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) &&
                    (props.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_4_BIT) &&
                    (props.sampledImageDepthSampleCounts & VK_SAMPLE_COUNT_4_BIT) &&
                    (props.sampledImageStencilSampleCounts & VK_SAMPLE_COUNT_4_BIT);
  REXGPU_INFO("[native] host 4x MSAA: {} (sample locations {}, cvar {})", msaa_supported_ ? "available" : "unavailable",
              vulkan_device_->extensions().ext_EXT_sample_locations ? "yes" : "no",
              REXCVAR_GET(ds2_native_msaa_host));
  REXGPU_INFO("[native] DS2 native renderer ready: {} shared memory binding(s), geometry shaders {}",
              shared_memory_bindings_, props.geometryShader ? "yes" : "no");
  return true;
}

void NativeCommandProcessor::ShutdownContext() {
  if (!vulkan_device_) return;
  const VkDevice dev = vulkan_device_->device();
  if (frame_open_) {
    EndRenderPass();
    dfn().vkEndCommandBuffer(slot().cmd);
    frame_open_ = false;
  }
  for (FrameSlot& s : slots_) {
    if (s.submitted) dfn().vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
  }
  for (auto& [k, img] : render_targets_) DestroyImage(img, false);
  for (auto& [k, t] : textures_) {
    for (auto& [s, v] : t.swizzle_views) DestroyView(v);
    DestroyImage(t.image, false);
  }
  for (auto& [k, r] : resolved_) {
    DestroyResolveLogicalImages(r, false);
    for (auto& [s, v] : r.swizzle_views) DestroyView(v);
    DestroyImage(r.image, false);
  }
  DestroyImage(white_, false);
  DestroyImage(white_cube_, false);
  DestroyImage(white_3d_, false);
  DestroyImage(color_resolve_scratch_, false);
  for (auto& [k, p] : convert_pipelines_) dfn().vkDestroyPipeline(dev, p, nullptr);
  for (auto& [k, p] : msaa_pipelines_) {
    if (p) dfn().vkDestroyPipeline(dev, p, nullptr);
  }
  for (VkShaderModule m : msaa_fs_) {
    if (m) dfn().vkDestroyShaderModule(dev, m, nullptr);
  }
  for (auto& [k, sh] : msaa_shadows_) DestroyImage(sh.image, false);
  msaa_shadows_.clear();
  for (auto& [k, h] : halfres_targets_) DestroyImage(h.image, false);
  halfres_targets_.clear();
  for (auto& [k, h] : halfres_depth_targets_) DestroyImage(h.image, false);
  halfres_depth_targets_.clear();
  if (halfres_composite_pipeline_) dfn().vkDestroyPipeline(dev, halfres_composite_pipeline_, nullptr);
  if (halfres_composite_pipeline_msaa_) dfn().vkDestroyPipeline(dev, halfres_composite_pipeline_msaa_, nullptr);
  if (halfres_composite_fs_) dfn().vkDestroyShaderModule(dev, halfres_composite_fs_, nullptr);
  if (linear_sampler_) dfn().vkDestroySampler(dev, linear_sampler_, nullptr);
  for (uint32_t i = 0; i < 2; ++i) {
    if (convert_layout_[i]) dfn().vkDestroyPipelineLayout(dev, convert_layout_[i], nullptr);
    if (convert_set_layout_[i]) dfn().vkDestroyDescriptorSetLayout(dev, convert_set_layout_[i], nullptr);
  }
  if (convert_sampler_) dfn().vkDestroySampler(dev, convert_sampler_, nullptr);
  for (auto& [k, fb] : present_framebuffers_) dfn().vkDestroyFramebuffer(dev, fb, nullptr);
  if (present_pipeline_) dfn().vkDestroyPipeline(dev, present_pipeline_, nullptr);
  if (present_pass_) dfn().vkDestroyRenderPass(dev, present_pass_, nullptr);
  if (present_layout_) dfn().vkDestroyPipelineLayout(dev, present_layout_, nullptr);
  if (present_set_layout_) dfn().vkDestroyDescriptorSetLayout(dev, present_set_layout_, nullptr);
  if (present_fs_) dfn().vkDestroyShaderModule(dev, present_fs_, nullptr);
  if (probe_fs_) dfn().vkDestroyShaderModule(dev, probe_fs_, nullptr);
  if (convert_vs_) dfn().vkDestroyShaderModule(dev, convert_vs_, nullptr);
  for (auto m : convert_fs_) {
    if (m) dfn().vkDestroyShaderModule(dev, m, nullptr);
  }
  StopPipelineThread();
  StopRestoreThread();
  for (auto& [k, p] : pipelines_) {
    if (p) dfn().vkDestroyPipeline(dev, p, nullptr);
  }
  for (auto& [k, f] : framebuffers_) dfn().vkDestroyFramebuffer(dev, f, nullptr);
  for (auto& [k, p] : render_passes_) dfn().vkDestroyRenderPass(dev, p, nullptr);
  for (auto& [k, s] : samplers_) dfn().vkDestroySampler(dev, s, nullptr);
  for (auto& [k, l] : pipeline_layouts_) dfn().vkDestroyPipelineLayout(dev, l.layout, nullptr);
  for (auto& [k, l] : texture_set_layouts_) dfn().vkDestroyDescriptorSetLayout(dev, l, nullptr);
  persistent_texture_sets_.clear();
  persistent_sets_by_view_.clear();
  for (VkDescriptorPool pool : persistent_pools_) dfn().vkDestroyDescriptorPool(dev, pool, nullptr);
  persistent_pools_.clear();
  for (FrameSlot& s : slots_) {
    for (VkFramebuffer fb : s.deferred_framebuffers) dfn().vkDestroyFramebuffer(dev, fb, nullptr);
    for (VkImageView v : s.deferred_views) DestroyView(v);
    for (auto& [i, m] : s.deferred_destroy) {
      dfn().vkDestroyImage(dev, i, nullptr);
      dfn().vkFreeMemory(dev, m, nullptr);
    }
    if (s.query_pool) dfn().vkDestroyQueryPool(dev, s.query_pool, nullptr);
    if (s.occlusion_pool) dfn().vkDestroyQueryPool(dev, s.occlusion_pool, nullptr);
    if (s.descriptor_pool) dfn().vkDestroyDescriptorPool(dev, s.descriptor_pool, nullptr);
    if (s.upload) dfn().vkDestroyBuffer(dev, s.upload, nullptr);
    if (s.upload_memory) dfn().vkFreeMemory(dev, s.upload_memory, nullptr);
    if (s.fence) dfn().vkDestroyFence(dev, s.fence, nullptr);
    if (s.pool) dfn().vkDestroyCommandPool(dev, s.pool, nullptr);
  }
  dfn().vkDestroyDescriptorSetLayout(dev, set_layout_shared_memory_, nullptr);
  dfn().vkDestroyDescriptorSetLayout(dev, set_layout_constants_, nullptr);
  dfn().vkDestroyDescriptorSetLayout(dev, set_layout_empty_, nullptr);
  shaders_.clear();
}

// --- Frames ----------------------------------------------------------------------------------------------------

bool NativeCommandProcessor::BeginFrame() {
  FrameSlot& s = slot();
  const VkDevice dev = vulkan_device_->device();
  if (s.submitted) {
    {
      ScopedUs t(stats_.us_fence_wait);
      dfn().vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
    }
    dfn().vkResetFences(dev, 1, &s.fence);
    s.submitted = false;
    CollectProfile(s);
    for (const auto& [address, index] : s.occlusion_pending) {
      uint64_t samples = 0;
      if (dfn().vkGetQueryPoolResults(dev, s.occlusion_pool, index, 1, sizeof(samples), &samples, sizeof(samples),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        NoteOcclusionResult(address, uint32_t(std::min<uint64_t>(samples, 0xFFFFFFFFu)));
      }
    }
  }
  s.occlusion_pending.clear();
  s.occlusion_used = 0;
  for (VkFramebuffer fb : s.deferred_framebuffers) dfn().vkDestroyFramebuffer(dev, fb, nullptr);
  s.deferred_framebuffers.clear();
  for (VkImageView v : s.deferred_views) DestroyView(v);
  s.deferred_views.clear();
  for (auto& [i, m] : s.deferred_destroy) {
    dfn().vkDestroyImage(dev, i, nullptr);
    dfn().vkFreeMemory(dev, m, nullptr);
  }
  s.deferred_destroy.clear();
  dfn().vkResetCommandPool(dev, s.pool, 0);
  s.texture_sets.clear();
  s.constant_sets.clear();
  dfn().vkResetDescriptorPool(dev, s.descriptor_pool, 0);
  s.upload_used = 0;
  s.content_cache.Reset();
  profile_shader_pipeline_ = VK_NULL_HANDLE;
  profile_shader_groups_ = 0;
  profile_shader_phase_groups_.fill(0);
  profile_shader_active_ = false;
  frame_vertex_uploads_.clear();
  // The frame's "shared memory" descriptor: every binding is the upload buffer.
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = s.descriptor_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &set_layout_shared_memory_;
  if (dfn().vkAllocateDescriptorSets(dev, &ai, &s.shared_memory_set) != VK_SUCCESS) return false;
  std::vector<VkDescriptorBufferInfo> infos(shared_memory_bindings_, {s.upload, 0, VK_WHOLE_SIZE});
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = s.shared_memory_set;
  w.dstBinding = 0;
  w.descriptorCount = shared_memory_bindings_;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = infos.data();
  dfn().vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn().vkBeginCommandBuffer(s.cmd, &bi) != VK_SUCCESS) return false;
  InvalidateDrawState();
  frame_open_ = true;
  work_since_barrier_ = true;
  s.queries = 0;
  s.query_labels.clear();
  s.seg_draws.clear();
  if (s.occlusion_pool) dfn().vkCmdResetQueryPool(s.cmd, s.occlusion_pool, 0, kMaxOcclusionQueries);
  if (profile_capable_) {
    const uint64_t every = uint64_t(std::max(REXCVAR_GET(ds2_native_gpu_profile_every), 1));
    profiling_ = (profile_counter_++ % every) == 0 || spike_frames_ > 0;
  }
  if (profiling_ && s.query_pool) {
    dfn().vkCmdResetQueryPool(s.cmd, s.query_pool, 0, kMaxQueries);
    Mark("other (between passes)");
  }
  return true;
}

void NativeCommandProcessor::Mark(const std::string& label) {
  if (!profiling_ || !frame_open_) return;
  FrameSlot& s = slot();
  if (!s.query_pool || s.queries + 1 >= kMaxQueries) return;  // one query kept for the end of the buffer
  uint16_t id;
  auto it = profile_label_ids_.find(label);
  if (it != profile_label_ids_.end()) {
    id = it->second;
  } else {
    id = uint16_t(profile_labels_.size());
    profile_labels_.push_back({label});
    profile_label_ids_.emplace(label, id);
  }
  dfn().vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.query_pool, s.queries++);
  s.query_labels.push_back(id);
  s.seg_draws.push_back(0);
}

const char* VkFormatShortName(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
      return "8888";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return "2_10_10_10";
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return "16f x4";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      return "11_11_10f";
    case VK_FORMAT_R16G16_SFLOAT:
      return "16f x2";
    case VK_FORMAT_R32_SFLOAT:
      return "32f";
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return "D24S8";
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return "D32FS8";
    default:
      return "fmt?";
  }
}

void NativeCommandProcessor::MarkPass(const VkFormat* color_formats, uint32_t color_count, VkFormat depth_format,
                                      uint32_t w, uint32_t h) {
  if (!profiling_) return;
  std::string l = fmt::format("pass {}x{}", w, h);
  for (uint32_t i = 0; i < color_count; ++i) l += fmt::format(" {}", VkFormatShortName(color_formats[i]));
  if (depth_format != VK_FORMAT_UNDEFINED) l += fmt::format(" +{}", VkFormatShortName(depth_format));
  Mark(l);
}

void NativeCommandProcessor::CollectProfile(FrameSlot& s) {
  if (!profile_capable_ || s.queries < 2) return;
  uint64_t ts[kMaxQueries];
  if (dfn().vkGetQueryPoolResults(vulkan_device_->device(), s.query_pool, 0, s.queries, sizeof(uint64_t) * s.queries,
                                  ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  for (uint32_t i = 0; i + 1 < s.queries; ++i) {
    const double ms = double((ts[i + 1] - ts[i]) & timestamp_mask_) * timestamp_period_ns_ * 1e-6;
    ProfileLabel& l = profile_labels_[s.query_labels[i]];
    l.ms += ms;
    ++l.count;
    l.draws += s.seg_draws[i];
  }
  const double buffer_ms = double((ts[s.queries - 1] - ts[0]) & timestamp_mask_) * timestamp_period_ns_ * 1e-6;
  profile_gpu_ms_ += buffer_ms;
  ++profile_buffers_;
  const int32_t spike_ms = REXCVAR_GET(ds2_native_gpu_spike_ms);
  if (spike_ms > 0 && buffer_ms >= double(spike_ms) && spike_logs_ < 6) {
    ++spike_logs_;
    std::unordered_map<uint16_t, std::pair<double, uint32_t>> per;
    for (uint32_t i = 0; i + 1 < s.queries; ++i) {
      auto& e = per[s.query_labels[i]];
      e.first += double((ts[i + 1] - ts[i]) & timestamp_mask_) * timestamp_period_ns_ * 1e-6;
      e.second += s.seg_draws[i];
    }
    std::vector<std::pair<uint16_t, std::pair<double, uint32_t>>> order(per.begin(), per.end());
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    std::string top;
    for (size_t i = 0; i < order.size() && i < 20; ++i) {
      top += fmt::format("\n[native gpu spike]   {:6.2f} ms  {} draws  {}", order[i].second.first, order[i].second.second,
                         profile_labels_[order[i].first].name);
    }
    REXGPU_WARN("[native gpu spike] command buffer took {:.1f} ms on the GPU:{}", buffer_ms, top);
  }
}

void NativeCommandProcessor::SubmitFrame() {
  if (!frame_open_) return;
  EndRenderPass();
  FrameSlot& s = slot();
  if (occlusion_active_ >= 0) {
    // A query cannot span command buffers: this one is dropped (the game gets the previous result).
    dfn().vkCmdEndQuery(s.cmd, s.occlusion_pool, uint32_t(occlusion_active_));
    occlusion_active_ = -1;
    ++stats_.occlusion_lost;
  }
  if (profiling_ && s.query_pool && s.queries > 0 && s.queries < kMaxQueries) {
    dfn().vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.query_pool, s.queries++);
    s.query_labels.push_back(0);
    s.seg_draws.push_back(0);
  }
  ++stats_.submits;
  dfn().vkEndCommandBuffer(s.cmd);
  if (s.upload_used) {
    ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, s.upload_memory, s.upload_memory_type, 0,
                                             s.upload_memory_size, s.upload_used);
  }
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &s.cmd;
  {
    auto queue = vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    if (dfn().vkQueueSubmit(queue.queue(), 1, &si, s.fence) != VK_SUCCESS) {
      REXGPU_ERROR("[native] vkQueueSubmit failed");
    }
  }
  s.submitted = true;
  frame_open_ = false;
  ++frame_;
  BeginFrame();
}

uint8_t* NativeCommandProcessor::Upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset_out) {
  FrameSlot& s = slot();
  VkDeviceSize offset = (s.upload_used + alignment - 1) & ~(alignment - 1);
  if (offset + size > kUploadBufferSize) {
    // Full: submit what is recorded (keeps the render pass state lost - the next draw begins it again).
    SubmitFrame();
    FrameSlot& n = slot();
    offset = 0;
    if (size > kUploadBufferSize) return nullptr;
    n.upload_used = size;
    offset_out = 0;
    return n.upload_mapping;
  }
  s.upload_used = offset + size;
  offset_out = offset;
  stats_.upload_bytes += size;
  return s.upload_mapping + offset;
}

bool NativeCommandProcessor::UploadImmutable(const void* data, size_t size, VkDeviceSize alignment,
                                              uint32_t tag, VkDeviceSize& offset) {
  if (!data || !size) return false;
  const bool cache = REXCVAR_GET(ds2_native_cpu_cache);
  const uint64_t hash = cache ? XXH3_64bits(data, size) : 0;
  uint64_t cached_offset;
  if (cache && slot().content_cache.Find(hash, tag, data, size, slot().upload_mapping,
                                         slot().upload_used, cached_offset) && cached_offset % alignment == 0) {
    offset = cached_offset;
    ++stats_.content_hits;
    stats_.content_saved_bytes += size;
    return true;
  }
  uint8_t* dst = Upload(size, alignment, offset);
  if (!dst) return false;
  std::memcpy(dst, data, size);
  if (cache) slot().content_cache.Insert(hash, tag, size, offset);
  return true;
}

// Logs the guest 256-entry gamma ramp whenever the game changes it (brightness setting, fades).
void NativeCommandProcessor::LogGammaRamp() {
  const auto* table = gamma_ramp_256_entry_table();
  const uint64_t hash = XXH3_64bits(table, 256 * 4);
  if (hash == last_gamma_ramp_hash_) return;
  last_gamma_ramp_hash_ = hash;
  std::string text;
  for (uint32_t i : {0u, 16u, 32u, 64u, 96u, 128u, 192u, 255u}) {
    uint32_t v;
    std::memcpy(&v, &table[i], 4);
    text += fmt::format(" {}:{}/{}/{}", i, (v >> 20) & 1023, (v >> 10) & 1023, v & 1023);
  }
  REXGPU_INFO("[native] gamma ramp (index:R/G/B of 1023){}; present gamma {:.2f}", text,
              REXCVAR_GET(ds2_native_present_gamma));
}

void NativeCommandProcessor::Barrier() {
  // RC66: on NVK every pipeline barrier is a wait-for-idle plus cache flushes. A barrier with no GPU work recorded
  // since the previous one adds nothing (the previous one already drained the pipe and flushed every cache).
  if (!work_since_barrier_ && REXCVAR_GET(ds2_native_barrier_elision)) {
    ++stats_.barriers_elided;
    return;
  }
  work_since_barrier_ = false;
  ++stats_.barriers_issued;
  VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  dfn().vkCmdPipelineBarrier(slot().cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             1, &b, 0, nullptr, 0, nullptr);
}

void NativeCommandProcessor::ResolveBarrier(NativeImage& src, NativeImage& dst, bool before) {
  if (!REXCVAR_GET(ds2_native_resolve_image_barriers) || src.image == dst.image) {
    Barrier();
    return;
  }
  if (!work_since_barrier_ && REXCVAR_GET(ds2_native_barrier_elision)) {
    ++stats_.barriers_elided;
    return;
  }
  work_since_barrier_ = false;
  ++stats_.barriers_issued;
  constexpr VkPipelineStageFlags stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT;
  constexpr VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  VkImageMemoryBarrier barriers[2] = {};
  NativeImage* images[2] = {&src, &dst};
  for (uint32_t i = 0; i < 2; ++i) {
    auto& b = barriers[i];
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = before ? access : (i ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT);
    b.dstAccessMask = before ? (i ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT) : access;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = images[i]->image;
    b.subresourceRange = {VkImageAspectFlags(images[i]->depth
                             ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                             : VK_IMAGE_ASPECT_COLOR_BIT), 0, images[i]->levels, 0, 1};
  }
  ++stats_.resolve_image_barriers;
  dfn().vkCmdPipelineBarrier(slot().cmd, before ? stages : VK_PIPELINE_STAGE_TRANSFER_BIT,
                           before ? VK_PIPELINE_STAGE_TRANSFER_BIT : stages, 0,
                           0, nullptr, 0, nullptr, 2, barriers);
}

void NativeCommandProcessor::EndRenderPass() {
  if (in_render_pass_) {
    dfn().vkCmdEndRenderPass(slot().cmd);
    if (profile_shader_active_) {
      Mark("other (after shader group)");
      profile_shader_active_ = false;
      profile_shader_pipeline_ = VK_NULL_HANDLE;
    }
    in_render_pass_ = false;
    current_framebuffer_key_ = 0;
  }
}

bool NativeCommandProcessor::CreateImage(NativeImage& img, VkFormat format, uint32_t w, uint32_t h, bool depth,
                                         VkImageUsageFlags usage, uint32_t levels, bool cube, uint32_t samples,
                                         uint32_t volume_depth) {
  VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = volume_depth ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  if (cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ci.format = format;
  ci.extent = {std::max(w, 1u), std::max(h, 1u), std::max(volume_depth, 1u)};
  ci.mipLevels = std::max(levels, 1u);
  ci.arrayLayers = cube ? 6 : 1;
  ci.samples = VkSampleCountFlagBits(samples);
  if (samples > 1 && depth && vulkan_device_->extensions().ext_EXT_sample_locations) {
    ci.flags |= VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
  }
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(vulkan_device_, ci,
                                                         ui::vulkan::util::MemoryPurpose::kDeviceLocal, img.image,
                                                         img.memory)) {
    REXGPU_ERROR("[native] image {}x{} format {} creation failed", w, h, uint32_t(format));
    return false;
  }
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = img.image;
  vci.viewType = volume_depth ? VK_IMAGE_VIEW_TYPE_3D : cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  vci.format = format;
  vci.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
      depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  if (depth) {
    // Attachments need both aspects.
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  }
  if (dfn().vkCreateImageView(vulkan_device_->device(), &vci, nullptr, &img.view) != VK_SUCCESS) {
    return false;
  }
  if (depth) {
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dfn().vkCreateImageView(vulkan_device_->device(), &vci, nullptr, &img.depth_view);
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    dfn().vkCreateImageView(vulkan_device_->device(), &vci, nullptr, &img.stencil_view);
  }
  img.format = format;
  img.width = ci.extent.width;
  img.height = ci.extent.height;
  img.levels = ci.mipLevels;
  img.layers = ci.arrayLayers;
  img.cube = cube;
  img.volume_depth = volume_depth;
  img.depth = depth;
  img.samples = samples;
  img.initialized = false;
  return true;
}

void NativeCommandProcessor::DestroyImage(NativeImage& img, bool deferred) {
  const VkDevice dev = vulkan_device_->device();
  // Views of a deferred image may still be referenced by recorded frames: destroyed with it, and so are the
  // framebuffers made on them (a new view could get the same handle value).
  for (VkImageView v : {img.view, img.depth_view, img.stencil_view}) {
    if (!v) continue;
    auto range = fb_by_view_.equal_range(v);
    for (auto it = range.first; it != range.second; ++it) {
      auto fit = framebuffers_.find(it->second);
      if (fit != framebuffers_.end()) {
        if (deferred) {
          slot().deferred_framebuffers.push_back(fit->second);
        } else {
          dfn().vkDestroyFramebuffer(dev, fit->second, nullptr);
        }
        framebuffers_.erase(fit);
      }
    }
    fb_by_view_.erase(v);
    if (current_framebuffer_key_ && in_render_pass_ == false) current_framebuffer_key_ = 0;
    if (deferred) {
      slot().deferred_views.push_back(v);
    } else {
      DestroyView(v);
    }
  }
  if (deferred && img.image) {
    slot().deferred_destroy.emplace_back(img.image, img.memory);
  } else {
    if (img.image) dfn().vkDestroyImage(dev, img.image, nullptr);
    if (img.memory) dfn().vkFreeMemory(dev, img.memory, nullptr);
  }
  img = NativeImage();
}

void NativeCommandProcessor::EnsureGeneral(NativeImage& img) {
  if (img.initialized) return;
  EndRenderPass();
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img.image;
  b.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
      img.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT);
  dfn().vkCmdPipelineBarrier(slot().cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
  work_since_barrier_ = true;
  img.initialized = true;
}

NativeImage* NativeCommandProcessor::GetRenderTarget(uint32_t base, uint32_t pitch, uint32_t format, bool depth,
                                                     uint32_t height_hint) {
  if (!depth) {
    // Same EDRAM bits: the blending-precision variants and gamma share one image (the shader converts gamma).
    auto storage = xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(format));
    if (storage == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) storage = xenos::ColorRenderTargetFormat::k_8_8_8_8;
    format = uint32_t(storage);
  }
  const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(format) << 28) | (uint64_t(depth) << 36);
  VkFormat vk_format;
  if (depth) {
    vk_format = xenos::DepthRenderTargetFormat(format) == xenos::DepthRenderTargetFormat::kD24S8
                    ? VK_FORMAT_D24_UNORM_S8_UINT
                    : VK_FORMAT_D32_SFLOAT_S8_UINT;
  } else {
    vk_format = ColorRtFormat(xenos::ColorRenderTargetFormat(format));
  }
  if (vk_format == VK_FORMAT_UNDEFINED || !pitch) return nullptr;
  uint32_t height = std::clamp<uint32_t>(rex::align<uint32_t>(height_hint, 8), 8, 2048);
  // v107: reduced internal resolution for the scene's render targets.
  const bool scaled = IsScaledPitch(pitch);
  const uint32_t image_width = scaled ? std::max<uint32_t>(Sc(pitch), 8) : pitch;
  if (scaled) height = std::max<uint32_t>(Sc(height), 8);
  auto it = render_targets_.find(key);
  if (it != render_targets_.end()) {
    if (it->second.height >= height) {
      AdoptRenderTarget(base, pitch, key, it->second);
      // RC71: any use other than a host 4x MSAA pass sees the sample-space image: bring the 4x contents back
      // first, and the 4x copy is stale once this image may be written.
      if (!msaa_lookup_) ExportMsaaShadow(it->second, true);
      return &it->second;
    }
    // Taller than before: recreate (the old contents are lost - logged, rare).
    REXGPU_WARN("[native] render target {}@{} grows {} -> {} rows", format, base, it->second.height, height);
    EndRenderPass();
    FlushHalfRes();  // v105: the pending composite may target this image or its 4x copy
    auto sit = msaa_shadows_.find(it->second.image);
    if (sit != msaa_shadows_.end()) {
      DestroyImage(sit->second.image, true);
      msaa_shadows_.erase(sit);
    }
    DestroyImage(it->second, true);
    render_targets_.erase(it);
  }
  NativeImage img;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (!CreateImage(img, vk_format, image_width, height, depth, usage)) return nullptr;
  EnsureGeneral(img);
  Written(img);
  img.scaled = scaled;
  // Start from zero like a fresh EDRAM range would look after a clear.
  VkImageSubresourceRange r = ui::vulkan::util::InitializeSubresourceRange(
      depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT);
  if (depth) {
    VkClearDepthStencilValue v = {0.0f, 0};
    auto clear_ds = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(vulkan_device_->device(),
                                                                            "vkCmdClearDepthStencilImage"));
    if (clear_ds) clear_ds(slot().cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
  } else {
    VkClearColorValue v = {};
    work_since_barrier_ = true;
    dfn().vkCmdClearColorImage(slot().cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
  }
  Barrier();
  REXGPU_INFO("[native] render target {} format {} at {} tiles: {}x{}", depth ? "depth" : "color", format, base,
              pitch, height);
  NativeImage& stored = render_targets_[key] = img;
  AdoptRenderTarget(base, pitch, key, stored);
  return &stored;
}

void NativeCommandProcessor::AdoptRenderTarget(uint32_t base, uint32_t pitch, uint64_t key, NativeImage& img) {
  const uint64_t area = uint64_t(base) | (uint64_t(pitch) << 12);
  auto it = edram_owner_.find(area);
  if (it == edram_owner_.end()) {
    edram_owner_.emplace(area, key);
    img.owned_before = true;
    return;
  }
  if (it->second == key) return;
  const uint64_t owner_key = it->second;
  it->second = key;
  auto oit = render_targets_.find(owner_key);
  if (oit == render_targets_.end()) return;
  NativeImage& owner = oit->second;
  // The EDRAM bits of the previous owner, seen in this format. Only the reinterpretations DS2 uses.
  const uint32_t owner_format = uint32_t((owner_key >> 28) & 0xFF);
  const uint32_t new_format = uint32_t((key >> 28) & 0xFF);
  const bool owner_depth = (owner_key >> 36) & 1, new_depth = (key >> 36) & 1;
  using F = xenos::ColorRenderTargetFormat;
  ConvertMode mode = kConvertModeCount;
  if (owner_depth && !new_depth && F(new_format) == F::k_8_8_8_8) {
    mode = kConvertDepthToRgba8;
  } else if (!owner_depth && !new_depth && F(owner_format) == F::k_2_10_10_10_FLOAT &&
             F(new_format) == F::k_2_10_10_10) {
    mode = kConvertFloat7e3ToUnorm10;
  } else if (!owner_depth && !new_depth && F(owner_format) == F::k_2_10_10_10 &&
             F(new_format) == F::k_2_10_10_10_FLOAT) {
    mode = kConvertUnorm10ToFloat7e3;
  }
  const bool first_time = !img.owned_before;
  img.owned_before = true;
  if (mode == kConvertModeCount) {
    // No conversion for this pair (8888 -> depth among them): this image received nothing. v110: count only what
    // it writes from now on, so converting it back (depth -> 8888) leaves the rows it never wrote to their owner's
    // pixels (v109 counted the whole image here, which kept the bottom strip).
    img.epoch_rows = 0;
    img.inherited_rows = 0;
    img.inherited_from = 0;
    return;
  }
  // RC71: the conversion reads the owner's sample-space image and writes this one.
  ExportMsaaShadow(owner, true);
  ExportMsaaShadow(img, true);
  // v109: only the rows the owner wrote (or received) while it owned the range; all of them for a new image.
  uint32_t rows = std::min(owner.height, img.height);
  if (!first_time && REXCVAR_GET(ds2_native_convert_written_rows)) {
    const uint32_t inherited = owner.inherited_from == key ? 0u : owner.inherited_rows;
    rows = std::min(rows, std::max(owner.epoch_rows, inherited));
  }
  img.epoch_rows = 0;
  img.inherited_rows = rows;
  img.inherited_from = owner_key;
  if (!rows) return;
  const VkRect2D rect = {{0, 0}, {std::min(owner.width, img.width), rows}};
  if (Tracing() && mode == kConvertDepthToRgba8) {
    REXGPU_INFO("[native trace]   convert depth->8888 at {}: before depth {:08X} stencil {:02X} color {:08X}", base,
                ReadPixel(owner, 100, 100, VK_IMAGE_ASPECT_DEPTH_BIT),
                ReadPixel(owner, 100, 100, VK_IMAGE_ASPECT_STENCIL_BIT),
                ReadPixel(img, 100, 100, VK_IMAGE_ASPECT_COLOR_BIT));
  }
  Convert(mode, owner, img, 0, 0, rect,
          owner_depth && xenos::DepthRenderTargetFormat(owner_format) == xenos::DepthRenderTargetFormat::kD24FS8);
  img.epoch_rows = 0;  // v110: received, not written (Convert marks its destination)
  if (Tracing()) {
    REXGPU_INFO("[native trace]   convert at {}: {} rows ({} written + {} inherited from another image)", base, rows,
                owner.epoch_rows, owner.inherited_from == key ? 0u : owner.inherited_rows);
  }
  if (Tracing() && mode == kConvertDepthToRgba8) {
    REXGPU_INFO("[native trace]   convert depth->8888 at {}: after color {:08X}", base,
                ReadPixel(img, 100, 100, VK_IMAGE_ASPECT_COLOR_BIT));
  }
}

bool NativeCommandProcessor::SetupConversions() {
  const VkDevice dev = vulkan_device_->device();
  std::vector<uint32_t> spirv;
  std::string error;
  if (!CompileGlsl(EShLangVertex, kConvertVertexShader, spirv, error)) {
    REXGPU_ERROR("[native] conversion vertex shader: {}", error);
    return false;
  }
  convert_vs_ = ui::vulkan::util::CreateShaderModule(vulkan_device_, spirv.data(), spirv.size() * 4);
  for (uint32_t m = 0; m < kConvertModeCount; ++m) {
    spirv.clear();
    if (!CompileGlsl(EShLangFragment, std::string(kConvertHeader) + kConvertBodies[m], spirv, error)) {
      REXGPU_ERROR("[native] conversion shader {}: {}", m, error);
      return false;
    }
    convert_fs_[m] = ui::vulkan::util::CreateShaderModule(vulkan_device_, spirv.data(), spirv.size() * 4);
  }
  for (uint32_t n = 0; n < 2; ++n) {
    VkDescriptorSetLayoutBinding b[2] = {};
    for (uint32_t i = 0; i <= n; ++i) {
      b[i].binding = i;
      b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = n + 1;
    ci.pBindings = b;
    if (dfn().vkCreateDescriptorSetLayout(dev, &ci, nullptr, &convert_set_layout_[n]) != VK_SUCCESS) return false;
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &convert_set_layout_[n];
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pc;
    if (dfn().vkCreatePipelineLayout(dev, &lci, nullptr, &convert_layout_[n]) != VK_SUCCESS) return false;
  }
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  dfn().vkCreateSampler(dev, &sci, nullptr, &convert_sampler_);
  // Presentation pass: frontbuffer -> presenter image, through the guest gamma ramp (as the emulation does).
  spirv.clear();
  if (!CompileGlsl(EShLangFragment, kPresentShader, spirv, error)) {
    REXGPU_ERROR("[native] present shader: {}", error);
    return false;
  }
  present_fs_ = ui::vulkan::util::CreateShaderModule(vulkan_device_, spirv.data(), spirv.size() * 4);
  if (REXCVAR_GET(ds2_native_probe) == 3) {
    spirv.clear();
    static const char kProbeFs[] = R"(#version 450
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
void main() { o0 = vec4(0.5, 0.5, 0.5, 1.0); o1 = o0; o2 = o0; o3 = o0; }
)";
    if (CompileGlsl(EShLangFragment, kProbeFs, spirv, error)) {
      probe_fs_ = ui::vulkan::util::CreateShaderModule(vulkan_device_, spirv.data(), spirv.size() * 4);
    } else {
      REXGPU_ERROR("[native] probe shader: {}", error);
    }
  }
  {
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 2;
    ci.pBindings = b;
    if (dfn().vkCreateDescriptorSetLayout(dev, &ci, nullptr, &present_set_layout_) != VK_SUCCESS) return false;
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};  // v116: + upscale and sharpening
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &present_set_layout_;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pc;
    if (dfn().vkCreatePipelineLayout(dev, &lci, nullptr, &present_layout_) != VK_SUCCESS) return false;
  }
  {
    VkAttachmentDescription att = {};
    att.format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1;
    rci.pAttachments = &att;
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    rci.dependencyCount = 2;
    rci.pDependencies = dep;
    if (dfn().vkCreateRenderPass(dev, &rci, nullptr, &present_pass_) != VK_SUCCESS) return false;
  }
  {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = convert_vs_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = present_fs_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = present_layout_;
    ci.renderPass = present_pass_;
    if (dfn().vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &present_pipeline_) != VK_SUCCESS) {
      return false;
    }
  }
  return convert_vs_ && convert_fs_[0] && convert_fs_[1] && convert_fs_[2];
}

NativeImage* NativeCommandProcessor::PrepareColorResolve(NativeImage& src, const VkRect2D& rect,
                                                          int32_t exp_bias) {
  if (!convert_vs_ || !convert_fs_[kConvertResolveColorRgba8]) return nullptr;
  const uint32_t width = uint32_t(rect.offset.x) + rect.extent.width;
  const uint32_t height = uint32_t(rect.offset.y) + rect.extent.height;
  if (!color_resolve_scratch_.image || color_resolve_scratch_.width < width ||
      color_resolve_scratch_.height < height) {
    EndRenderPass();
    DestroyImage(color_resolve_scratch_, true);
    if (!CreateImage(color_resolve_scratch_, VK_FORMAT_R8G8B8A8_UNORM, width, height, false,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      DestroyImage(color_resolve_scratch_, false);
      return nullptr;
    }
    EnsureGeneral(color_resolve_scratch_);
  }
  // Do not change the resolve entry's format/generation until conversion succeeds.
  // If pipeline/descriptor/framebuffer creation fails, IssueCopy uses its original source.
  if (!Convert(kConvertResolveColorRgba8, src, color_resolve_scratch_, rect.offset.x, rect.offset.y,
               rect, false, exp_bias)) {
    REXGPU_WARN("[native] RGBA8 resolve conversion unavailable, keeping the original native copy path");
    return nullptr;
  }
  return &color_resolve_scratch_;
}

bool NativeCommandProcessor::Convert(ConvertMode mode, NativeImage& src, NativeImage& dst, int32_t src_x,
                                     int32_t src_y, const VkRect2D& dst_rect, bool src_float24,
                                     int32_t resolve_exp_bias) {
  Written(dst);
  dst.Wrote(uint32_t(dst_rect.offset.y) + dst_rect.extent.height);
  if (!convert_vs_ || !convert_fs_[mode] || !dst_rect.extent.width || !dst_rect.extent.height) return false;
  const VkDevice dev = vulkan_device_->device();
  VkFormat dst_format = dst.format;
  VkRenderPass pass = GetRenderPass(&dst_format, 1, VK_FORMAT_UNDEFINED);
  if (!pass) return false;
  const uint32_t layout_index = mode == kConvertDepthToRgba8 ? 1 : 0;
  const uint64_t pkey = uint64_t(mode) | (uint64_t(dst_format) << 8);
  VkPipeline pipeline;
  auto pit = convert_pipelines_.find(pkey);
  if (pit != convert_pipelines_.end()) {
    pipeline = pit->second;
  } else {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = convert_vs_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = convert_fs_[mode];
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = convert_layout_[layout_index];
    ci.renderPass = pass;
    if (dfn().vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
    }
    convert_pipelines_.emplace(pkey, pipeline);
  }
  if (!pipeline) return false;
  // Descriptors.
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = slot().descriptor_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &convert_set_layout_[layout_index];
  VkDescriptorSet set;
  if (dfn().vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo ii[2] = {};
  ii[0] = {convert_sampler_, src.depth ? src.depth_view : src.view, VK_IMAGE_LAYOUT_GENERAL};
  ii[1] = {convert_sampler_, src.stencil_view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[2] = {};
  for (uint32_t i = 0; i <= layout_index; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = set;
    w[i].dstBinding = i;
    w[i].descriptorCount = 1;
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].pImageInfo = &ii[i];
  }
  dfn().vkUpdateDescriptorSets(dev, layout_index + 1, w, 0, nullptr);
  // Framebuffer on the destination.
  uint64_t fb_key = (reinterpret_cast<uint64_t>(pass) * 1000003u + reinterpret_cast<uint64_t>(dst.view)) * 31 +
                    dst.width * 4099 + dst.height;
  VkFramebuffer fb;
  auto fit = framebuffers_.find(fb_key);
  if (fit != framebuffers_.end()) {
    fb = fit->second;
  } else {
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &dst.view;
    fci.width = dst.width;
    fci.height = dst.height;
    fci.layers = 1;
    if (dfn().vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) return false;
    framebuffers_.emplace(fb_key, fb);
    fb_by_view_.emplace(dst.view, fb_key);
  }
  EndRenderPass();
  Barrier();
  if (profiling_) Mark(fmt::format("convert mode {} {}x{}", uint32_t(mode), dst.width, dst.height));
  VkCommandBuffer cmd = slot().cmd;
  VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = pass;
  bi.framebuffer = fb;
  bi.renderArea = {{0, 0}, {dst.width, dst.height}};
  work_since_barrier_ = true;
  InvalidateDrawState();
  dfn().vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convert_layout_[layout_index], 0, 1, &set, 0,
                                nullptr);
  const int32_t pc[4] = {src_x - dst_rect.offset.x, src_y - dst_rect.offset.y,
                         src_float24 ? (REXCVAR_GET(ds2_native_float24_round) ? 1 : 2) : 0, resolve_exp_bias};
  dfn().vkCmdPushConstants(cmd, convert_layout_[layout_index], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
  VkViewport vp = {float(dst_rect.offset.x), float(dst_rect.offset.y), float(dst_rect.extent.width),
                   float(dst_rect.extent.height), 0.0f, 1.0f};
  dfn().vkCmdSetViewport(cmd, 0, 1, &vp);
  dfn().vkCmdSetScissor(cmd, 0, 1, &dst_rect);
  work_since_barrier_ = true;
  dfn().vkCmdDraw(cmd, 3, 1, 0, 0);
  dfn().vkCmdEndRenderPass(cmd);
  Barrier();
  ++conversions_;
  ++stats_.conversions;
  return true;
}

// --- RC71: host 4x MSAA ------------------------------------------------------------------------------------------

// Sample s of pixel (x, y) of the 4x image is pixel (2x + (s & 1), 2y + (s >> 1)) of the sample-space image (the
// pipelines place the 4 samples at the Xenos ordered-grid positions, so both hold the same values).
enum MsaaTransferMode : uint32_t {
  kMsaaColorToMs,
  kMsaaColorFromMs,
  kMsaaDepthToMs,
  kMsaaDepthFromMs,
  kMsaaStencilToMs,
  kMsaaStencilFromMs,
  kMsaaDepthFloat24,  // RC76: 1x depth -> 1x depth rounded toward zero to float24 (depth resolves of D24FS8)
  kMsaaDepthDownsample,  // RC87: depth at 1/divisor resolution, nearest (max) or farthest (min) of each block
  kMsaaTransferModeCount,
};

constexpr const char* kMsaaHeader = R"(#version 450
layout(push_constant) uniform P { int bit; int dx; int dy; } pc;
ivec2 SampleSpaceCoord() { return ivec2(gl_FragCoord.xy) * 2 + ivec2(gl_SampleID & 1, gl_SampleID >> 1); }
int SampleOfPixel(ivec2 q) { return (q.x & 1) | ((q.y & 1) << 1); }
)";

constexpr const char* kMsaaBodies[kMsaaTransferModeCount] = {
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(t, SampleSpaceCoord(), 0); }
)",
    R"(layout(set = 0, binding = 0) uniform sampler2DMS t;
layout(location = 0) out vec4 o;
void main() { ivec2 q = ivec2(gl_FragCoord.xy); o = texelFetch(t, q >> 1, SampleOfPixel(q)); }
)",
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
void main() { gl_FragDepth = texelFetch(t, SampleSpaceCoord(), 0).r; }
)",
    R"(layout(set = 0, binding = 0) uniform sampler2DMS t;
void main() { ivec2 q = ivec2(gl_FragCoord.xy); gl_FragDepth = texelFetch(t, q >> 1, SampleOfPixel(q)).r; }
)",
    R"(layout(set = 0, binding = 0) uniform usampler2D t;
void main() { if (((texelFetch(t, SampleSpaceCoord(), 0).r >> uint(pc.bit)) & 1u) == 0u) discard; }
)",
    R"(layout(set = 0, binding = 0) uniform usampler2DMS t;
void main() {
  ivec2 q = ivec2(gl_FragCoord.xy);
  if (((texelFetch(t, q >> 1, SampleOfPixel(q)).r >> uint(pc.bit)) & 1u) == 0u) discard;
}
)",
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
// The guest stores D24FS8 depth as 20e4 float24; the host depth buffer is float32. The value a guest shader samples
// is the float32 depth truncated to float24 (as the emulation's resolves, depth_float24_round = false).
void main() {
  float z = texelFetch(t, ivec2(gl_FragCoord.xy) + ivec2(pc.dx, pc.dy), 0).r;
  uint f = 0u;
  if (z > 0.0) {
    uint u = floatBitsToUint(z);
    if (u >= 0x3FFFFFF8u) {
      f = 0xFFFFFFu;
    } else {
      if (u < 0x38800000u) {
        uint sh = min(113u - (u >> 23), 24u);
        u = (0x800000u | (u & 0x7FFFFFu)) >> sh;
      } else {
        u += 0xC8000000u;
      }
      f = (u >> 3) & 0xFFFFFFu;
    }
  }
  float r = 0.0;
  if (f != 0u) {
    uint m = f & 0xFFFFFu, e = f >> 20;
    if (e == 0u) {
      uint lz = 31u - uint(findMSB(m)) - 11u;
      e = uint(1 - int(lz));
      m = (m << lz) & 0xFFFFFu;
    }
    r = uintBitsToFloat(((e + 112u) << 23) | (m << 3));
  }
  gl_FragDepth = r;
}
)",
    R"(layout(set = 0, binding = 0) uniform sampler2D t;
void main() {
  ivec2 size = textureSize(t, 0) - 1;
  ivec2 b = ivec2(gl_FragCoord.xy) * pc.bit;
  float m = pc.dx != 0 ? 0.0 : 1.0;
  for (int y = 0; y < pc.bit; ++y) {
    for (int x = 0; x < pc.bit; ++x) {
      float z = texelFetch(t, min(b + ivec2(x, y), size), 0).r;
      m = pc.dx != 0 ? max(m, z) : min(m, z);
    }
  }
  gl_FragDepth = m;
}
)"
};

VkPipeline NativeCommandProcessor::GetMsaaPipeline(uint32_t mode, VkFormat format, bool ms, VkRenderPass pass) {
  const uint64_t key = uint64_t(mode) | (uint64_t(format) << 8) | (uint64_t(ms) << 40);
  auto it = msaa_pipelines_.find(key);
  if (it != msaa_pipelines_.end()) return it->second;
  const VkDevice dev = vulkan_device_->device();
  if (!msaa_fs_[mode]) {
    std::vector<uint32_t> spirv;
    std::string error;
    if (!CompileGlsl(EShLangFragment, std::string(kMsaaHeader) + kMsaaBodies[mode], spirv, error)) {
      REXGPU_ERROR("[native] MSAA transfer shader {}: {}", mode, error);
      msaa_pipelines_.emplace(key, VK_NULL_HANDLE);
      return VK_NULL_HANDLE;
    }
    msaa_fs_[mode] = ui::vulkan::util::CreateShaderModule(vulkan_device_, spirv.data(), spirv.size() * 4);
  }
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = convert_vs_;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = msaa_fs_[mode];
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  static const VkSampleLocationEXT kXenosLocations[4] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f},
                                                        {0.75f, 0.75f}};
  VkPipelineSampleLocationsStateCreateInfoEXT msl = {
      VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT};
  VkPipelineMultisampleStateCreateInfo msi = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  msi.rasterizationSamples = ms ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
  if (ms) {
    msi.sampleShadingEnable = VK_TRUE;
    msi.minSampleShading = 1.0f;
    msl.sampleLocationsEnable = VK_TRUE;
    msl.sampleLocationsInfo.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
    msl.sampleLocationsInfo.sampleLocationsPerPixel = VK_SAMPLE_COUNT_4_BIT;
    msl.sampleLocationsInfo.sampleLocationGridSize = {1, 1};
    msl.sampleLocationsInfo.sampleLocationsCount = 4;
    msl.sampleLocationsInfo.pSampleLocations = kXenosLocations;
    msi.pNext = &msl;
  }
  const bool color = mode == kMsaaColorToMs || mode == kMsaaColorFromMs;
  const bool stencil = mode == kMsaaStencilToMs || mode == kMsaaStencilFromMs;
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  if (!color && !stencil) {
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  }
  if (stencil) {
    ds.stencilTestEnable = VK_TRUE;
    ds.front.failOp = ds.front.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.front.passOp = VK_STENCIL_OP_REPLACE;
    ds.front.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.back = ds.front;
  }
  VkPipelineColorBlendAttachmentState ba = {};
  ba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = color ? 1 : 0;
  cb.pAttachments = &ba;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                          VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = stencil ? 5 : 2;
  dy.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &msi;
  ci.pDepthStencilState = color ? nullptr : &ds;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dy;
  ci.layout = convert_layout_[0];
  ci.renderPass = pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn().vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
    REXGPU_ERROR("[native] MSAA transfer pipeline {} format {} failed", mode, uint32_t(format));
    pipeline = VK_NULL_HANDLE;
  }
  msaa_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool NativeCommandProcessor::MsaaTransfer(NativeImage& rt, NativeImage& ms, bool to_ms, uint32_t rows) {
  NativeImage& dst = to_ms ? ms : rt;
  NativeImage& src = to_ms ? rt : ms;
  Written(dst);
  dst.Wrote(rows ? rows : dst.height);
  const VkDevice dev = vulkan_device_->device();
  const VkFormat format = dst.format;
  const uint32_t samples = to_ms ? 4 : 1;
  VkRenderPass pass = dst.depth ? GetRenderPass(nullptr, 0, format, samples) : GetRenderPass(&format, 1,
                                                                                          VK_FORMAT_UNDEFINED, samples);
  if (!pass) return false;
  const uint32_t main_mode = dst.depth ? (to_ms ? kMsaaDepthToMs : kMsaaDepthFromMs)
                                       : (to_ms ? kMsaaColorToMs : kMsaaColorFromMs);
  VkPipeline main_pipeline = GetMsaaPipeline(main_mode, format, to_ms, pass);
  VkPipeline stencil_pipeline =
      dst.depth ? GetMsaaPipeline(to_ms ? kMsaaStencilToMs : kMsaaStencilFromMs, format, to_ms, pass) : VK_NULL_HANDLE;
  if (!main_pipeline || (dst.depth && !stencil_pipeline)) {
    REXGPU_WARN("[native] host 4x MSAA: transfer pipeline unavailable (mode {})", main_mode);
    return false;
  }
  VkDescriptorSet sets[2] = {};
  VkImageView views[2] = {dst.depth ? src.depth_view : src.view, src.stencil_view};
  const uint32_t set_count = dst.depth ? 2 : 1;
  for (uint32_t i = 0; i < set_count; ++i) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = slot().descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &convert_set_layout_[0];
    if (dfn().vkAllocateDescriptorSets(dev, &ai, &sets[i]) != VK_SUCCESS) {
      REXGPU_WARN("[native] host 4x MSAA: transfer descriptor set unavailable");
      return false;
    }
    VkDescriptorImageInfo ii = {convert_sampler_, views[i], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets[i];
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    dfn().vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  }
  uint64_t fb_key = (reinterpret_cast<uint64_t>(pass) * 1000003u + reinterpret_cast<uint64_t>(dst.view)) * 31 +
                    dst.width * 4099 + dst.height;
  VkFramebuffer fb;
  auto fit = framebuffers_.find(fb_key);
  if (fit != framebuffers_.end()) {
    fb = fit->second;
  } else {
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &dst.view;
    fci.width = dst.width;
    fci.height = dst.height;
    fci.layers = 1;
    if (dfn().vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) return false;
    framebuffers_.emplace(fb_key, fb);
    fb_by_view_.emplace(dst.view, fb_key);
  }
  EndRenderPass();
  Barrier();
  if (profiling_) Mark(fmt::format("msaa {} {} {}x{}", to_ms ? "import" : "export", dst.depth ? "depth" : "color",
                                   dst.width, dst.height));
  VkCommandBuffer cmd = slot().cmd;
  // RC72: only the rows that changed (the whole image when unknown).
  const uint32_t dst_rows = rows ? std::min(rows, dst.height) : dst.height;
  VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = pass;
  bi.framebuffer = fb;
  bi.renderArea = {{0, 0}, {dst.width, dst_rows}};
  work_since_barrier_ = true;
  InvalidateDrawState();
  dfn().vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport vp = {0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f};
  const VkRect2D sr = {{0, 0}, {dst.width, dst_rows}};
  dfn().vkCmdSetViewport(cmd, 0, 1, &vp);
  dfn().vkCmdSetScissor(cmd, 0, 1, &sr);
  int32_t pc[4] = {};
  dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, main_pipeline);
  dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convert_layout_[0], 0, 1, &sets[0], 0, nullptr);
  dfn().vkCmdPushConstants(cmd, convert_layout_[0], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
  dfn().vkCmdDraw(cmd, 3, 1, 0, 0);
  if (dst.depth) {
    // No stencil export on NVK: clear the stencil, then set it bit by bit (the shader keeps the samples that have
    // the bit, the stencil test replaces with 0xFF through a one-bit write mask).
    VkClearAttachment ca = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
    ca.clearValue.depthStencil.stencil = 0;
    VkClearRect cr = {sr, 0, 1};
    dfn().vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, stencil_pipeline);
    dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convert_layout_[0], 0, 1, &sets[1], 0,
                                  nullptr);
    dfn().vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    dfn().vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    for (int32_t bit = 0; bit < 8; ++bit) {
      pc[0] = bit;
      dfn().vkCmdPushConstants(cmd, convert_layout_[0], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
      dfn().vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
      dfn().vkCmdDraw(cmd, 3, 1, 0, 0);
    }
  }
  dfn().vkCmdEndRenderPass(cmd);
  work_since_barrier_ = true;
  Barrier();
  InvalidateDrawState();
  if (to_ms) {
    ++stats_.msaa_imports;
  } else {
    ++stats_.msaa_exports;
  }
  return true;
}

// RC76: the depth of a D24FS8 resolve as the guest texture holds it (float24, rounded toward zero), written over the
// copied depth of dst_rect. Point-light shadow cubes compare the receiver depth to it with sge and no bias: with the
// exact float32 depth the lit wall shadows itself at random (striped squares in torch light, flickering).
bool NativeCommandProcessor::QuantizeDepthFloat24(NativeImage& src, NativeImage& dst, int32_t src_x, int32_t src_y,
                                                  const VkRect2D& dst_rect) {
  Written(dst);
  if (!src.depth || !dst.depth || !src.depth_view || !dst_rect.extent.width || !dst_rect.extent.height) return false;
  const VkDevice dev = vulkan_device_->device();
  VkRenderPass pass = GetRenderPass(nullptr, 0, dst.format, 1);
  if (!pass) return false;
  VkPipeline pipeline = GetMsaaPipeline(kMsaaDepthFloat24, dst.format, false, pass);
  if (!pipeline) return false;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = slot().descriptor_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &convert_set_layout_[0];
  if (dfn().vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo ii = {convert_sampler_, src.depth_view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &ii;
  dfn().vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  uint64_t fb_key = (reinterpret_cast<uint64_t>(pass) * 1000003u + reinterpret_cast<uint64_t>(dst.view)) * 31 +
                    dst.width * 4099 + dst.height;
  VkFramebuffer fb;
  auto fit = framebuffers_.find(fb_key);
  if (fit != framebuffers_.end()) {
    fb = fit->second;
  } else {
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &dst.view;
    fci.width = dst.width;
    fci.height = dst.height;
    fci.layers = 1;
    if (dfn().vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) return false;
    framebuffers_.emplace(fb_key, fb);
    fb_by_view_.emplace(dst.view, fb_key);
  }
  EndRenderPass();
  Barrier();
  if (profiling_) Mark(fmt::format("float24 depth {}x{}", dst_rect.extent.width, dst_rect.extent.height));
  VkCommandBuffer cmd = slot().cmd;
  VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = pass;
  bi.framebuffer = fb;
  bi.renderArea = dst_rect;
  work_since_barrier_ = true;
  InvalidateDrawState();
  dfn().vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport vp = {0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f};
  dfn().vkCmdSetViewport(cmd, 0, 1, &vp);
  dfn().vkCmdSetScissor(cmd, 0, 1, &dst_rect);
  const int32_t pc[4] = {0, src_x - dst_rect.offset.x, src_y - dst_rect.offset.y, 0};
  dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convert_layout_[0], 0, 1, &set, 0, nullptr);
  dfn().vkCmdPushConstants(cmd, convert_layout_[0], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
  dfn().vkCmdDraw(cmd, 3, 1, 0, 0);
  dfn().vkCmdEndRenderPass(cmd);
  work_since_barrier_ = true;
  Barrier();
  InvalidateDrawState();
  ++stats_.depth_float24_quantized;
  return true;
}

uint32_t NativeCommandProcessor::HalfResMode(uint64_t hash) {
  const std::string& list = REXCVAR_GET(ds2_native_halfres_ps);
  if (list.empty()) return 0;
  const std::string h = fmt::format("{:016X}", hash);
  const size_t at = list.find(h);
  if (at == std::string::npos) return 0;
  // RC87: "HASH:z" = particles that are not soft (no scene depth in the shader): drawn with a reduced depth buffer,
  // no pixel-position constant change. Plain "HASH" = soft particles (pixel position scaled, no depth buffer).
  return (at + 18 <= list.size() && list.compare(at + 16, 2, ":z") == 0) ? 2u : 1u;
}

bool NativeCommandProcessor::IsHalfResPixelShader(uint64_t hash) { return HalfResMode(hash) != 0; }

bool NativeCommandProcessor::DownsampleDepth(NativeImage& src, NativeImage& dst, uint32_t divisor, bool use_max) {
  Written(dst);
  if (!src.depth || !dst.depth || !src.depth_view) return false;
  const VkDevice dev = vulkan_device_->device();
  VkRenderPass pass = GetRenderPass(nullptr, 0, dst.format, 1);
  if (!pass) return false;
  VkPipeline pipeline = GetMsaaPipeline(kMsaaDepthDownsample, dst.format, false, pass);
  if (!pipeline) return false;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = slot().descriptor_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &convert_set_layout_[0];
  if (dfn().vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo ii = {convert_sampler_, src.depth_view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &ii;
  dfn().vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  uint64_t fb_key = (reinterpret_cast<uint64_t>(pass) * 1000003u + reinterpret_cast<uint64_t>(dst.view)) * 31 +
                    dst.width * 4099 + dst.height;
  VkFramebuffer fb;
  auto fit = framebuffers_.find(fb_key);
  if (fit != framebuffers_.end()) {
    fb = fit->second;
  } else {
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &dst.view;
    fci.width = dst.width;
    fci.height = dst.height;
    fci.layers = 1;
    if (dfn().vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) return false;
    framebuffers_.emplace(fb_key, fb);
    fb_by_view_.emplace(dst.view, fb_key);
  }
  EndRenderPass();
  Barrier();
  if (profiling_) Mark(fmt::format("reduced depth {}x{}", dst.width, dst.height));
  VkCommandBuffer cmd = slot().cmd;
  VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = pass;
  bi.framebuffer = fb;
  bi.renderArea = {{0, 0}, {dst.width, dst.height}};
  work_since_barrier_ = true;
  InvalidateDrawState();
  dfn().vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport vp = {0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f};
  const VkRect2D sr = {{0, 0}, {dst.width, dst.height}};
  dfn().vkCmdSetViewport(cmd, 0, 1, &vp);
  dfn().vkCmdSetScissor(cmd, 0, 1, &sr);
  const int32_t pc[4] = {int32_t(divisor), use_max ? 1 : 0, 0, 0};
  dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convert_layout_[0], 0, 1, &set, 0, nullptr);
  dfn().vkCmdPushConstants(cmd, convert_layout_[0], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
  dfn().vkCmdDraw(cmd, 3, 1, 0, 0);
  dfn().vkCmdEndRenderPass(cmd);
  work_since_barrier_ = true;
  Barrier();
  InvalidateDrawState();
  ++stats_.halfres_depth_downsamples;
  return true;
}

bool NativeCommandProcessor::CompositeHalfRes(NativeImage& rt, NativeImage& half, const VkRect2D& rect) {
  Written(rt);
  rt.Wrote(uint32_t(rect.offset.y) + rect.extent.height);
  const VkDevice dev = vulkan_device_->device();
  VkFormat format = rt.format;
  const bool msaa = rt.samples == 4;
  VkRenderPass pass = GetRenderPass(&format, 1, VK_FORMAT_UNDEFINED, msaa ? 4 : 1);
  if (!pass) return false;
  VkPipeline& composite_pipeline = msaa ? halfres_composite_pipeline_msaa_ : halfres_composite_pipeline_;
  if (!linear_sampler_) {
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn().vkCreateSampler(dev, &sci, nullptr, &linear_sampler_) != VK_SUCCESS) return false;
  }
  if (!composite_pipeline) {
    static const char* kFs = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(push_constant) uniform P { vec2 inv_size; float scale; } pc;
layout(location = 0) out vec4 o;
void main() {
  vec4 c = texture(t, gl_FragCoord.xy * pc.inv_size);
  // rgb: accumulated particle color; a: what remains of the scene behind them (1 = untouched).
  o = vec4(c.rgb * pc.scale, 1.0 - pc.scale * (1.0 - c.a));
}
)";
    std::vector<uint32_t> spirv;
    std::string error;
    if (!CompileGlsl(EShLangFragment, kFs, spirv, error)) {
      REXGPU_ERROR("[native] half-resolution composite shader: {}", error);
      return false;
    }
    if (!halfres_composite_fs_) {
      halfres_composite_fs_ = ui::vulkan::util::CreateShaderModule(vulkan_device_, spirv.data(), spirv.size() * 4);
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = convert_vs_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = halfres_composite_fs_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo msi = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msi.rasterizationSamples = msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &msi;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = convert_layout_[0];
    ci.renderPass = pass;
    if (dfn().vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &composite_pipeline) != VK_SUCCESS) {
      composite_pipeline = VK_NULL_HANDLE;
      return false;
    }
  }
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = slot().descriptor_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &convert_set_layout_[0];
  if (dfn().vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo ii = {linear_sampler_, half.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &ii;
  dfn().vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  uint64_t fb_key = (reinterpret_cast<uint64_t>(pass) * 1000003u + reinterpret_cast<uint64_t>(rt.view)) * 31 +
                    rt.width * 4099 + rt.height;
  VkFramebuffer fb;
  auto fit = framebuffers_.find(fb_key);
  if (fit != framebuffers_.end()) {
    fb = fit->second;
  } else {
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &rt.view;
    fci.width = rt.width;
    fci.height = rt.height;
    fci.layers = 1;
    if (dfn().vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) return false;
    framebuffers_.emplace(fb_key, fb);
    fb_by_view_.emplace(rt.view, fb_key);
  }
  EndRenderPass();
  Barrier();
  if (profiling_) Mark("half-resolution composite");
  VkCommandBuffer cmd = slot().cmd;
  VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = pass;
  bi.framebuffer = fb;
  bi.renderArea = rect;
  work_since_barrier_ = true;
  InvalidateDrawState();
  dfn().vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport vp = {0.0f, 0.0f, float(rt.width), float(rt.height), 0.0f, 1.0f};
  dfn().vkCmdSetViewport(cmd, 0, 1, &vp);
  dfn().vkCmdSetScissor(cmd, 0, 1, &rect);
  const float pc[4] = {1.0f / float(rt.width), 1.0f / float(rt.height),
                       float(std::clamp(REXCVAR_GET(ds2_native_halfres_strength), 0.0, 4.0)), 0.0f};
  dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline);
  dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convert_layout_[0], 0, 1, &set, 0, nullptr);
  dfn().vkCmdPushConstants(cmd, convert_layout_[0], VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
  dfn().vkCmdDraw(cmd, 3, 1, 0, 0);
  dfn().vkCmdEndRenderPass(cmd);
  work_since_barrier_ = true;
  Barrier();
  InvalidateDrawState();
  ++stats_.halfres_composites;
  return true;
}

void NativeCommandProcessor::FlushHalfRes() {
  if (!halfres_pending_rt_) return;
  NativeImage* rt = halfres_pending_rt_;
  NativeImage* half = halfres_pending_half_;
  halfres_pending_rt_ = nullptr;
  halfres_pending_half_ = nullptr;
  if (halfres_pending_rect_.extent.width && halfres_pending_rect_.extent.height) {
    CompositeHalfRes(*rt, *half, halfres_pending_rect_);
  }
}

void NativeCommandProcessor::DestroyView(VkImageView view) {
  if (!view) return;
  auto it = persistent_sets_by_view_.find(view);
  if (it != persistent_sets_by_view_.end()) {
    // The view was retired a full frame ring ago (or the GPU is idle): no pending work uses these sets.
    for (const auto& key : it->second) {
      auto sit = persistent_texture_sets_.find(key);
      if (sit == persistent_texture_sets_.end()) continue;
      free_descriptor_sets_(vulkan_device_->device(), sit->second.pool, 1, &sit->second.set);
      persistent_texture_sets_.erase(sit);
      ++stats_.texture_sets_dropped;
    }
    persistent_sets_by_view_.erase(it);
  }
  dfn().vkDestroyImageView(vulkan_device_->device(), view, nullptr);
}

bool NativeCommandProcessor::AllocatePersistentSet(VkDescriptorSetLayout layout, PersistentSet& out) {
  const VkDevice dev = vulkan_device_->device();
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  for (size_t i = persistent_pools_.size(); i-- > 0;) {
    ai.descriptorPool = persistent_pools_[i];
    if (dfn().vkAllocateDescriptorSets(dev, &ai, &out.set) == VK_SUCCESS) {
      out.pool = persistent_pools_[i];
      return true;
    }
  }
  if (persistent_pools_.size() >= 16) return false;
  constexpr uint32_t kSets = 4096;
  const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets * 6},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, kSets * 6}};
  VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  ci.maxSets = kSets;
  ci.poolSizeCount = 2;
  ci.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (dfn().vkCreateDescriptorPool(dev, &ci, nullptr, &pool) != VK_SUCCESS) return false;
  persistent_pools_.push_back(pool);
  ai.descriptorPool = pool;
  if (dfn().vkAllocateDescriptorSets(dev, &ai, &out.set) != VK_SUCCESS) return false;
  out.pool = pool;
  return true;
}

NativeImage* NativeCommandProcessor::GetMsaaShadow(NativeImage& rt, bool import, uint32_t import_rows) {
  MsaaShadow& sh = msaa_shadows_[rt.image];
  if (!sh.image.image) {
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        (rt.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(sh.image, rt.format, rt.width / 2, rt.height / 2, rt.depth, usage, 1, false, 4)) {
      DestroyImage(sh.image, false);
      msaa_shadows_.erase(rt.image);
      return nullptr;
    }
    EnsureGeneral(sh.image);
    sh.valid = false;
    sh.newer = false;
  }
  if (!sh.valid) {
    if (import) {
      if (!MsaaTransfer(rt, sh.image, true, import_rows)) {
        REXGPU_WARN("[native] host 4x MSAA: import of a {}x{} {} target failed; sample space used", rt.width,
                    rt.height, rt.depth ? "depth" : "color");
        return nullptr;
      }
    } else {
      ++stats_.msaa_imports_skipped;
    }
    sh.valid = true;
  }
  return &sh.image;
}

void NativeCommandProcessor::ExportMsaaShadow(NativeImage& rt, bool invalidate) {
  if (msaa_shadows_.empty()) return;
  auto it = msaa_shadows_.find(rt.image);
  if (it == msaa_shadows_.end()) return;
  MsaaShadow& sh = it->second;
  // v105: reduced-resolution particles pending for this 4x copy are composited into it first.
  if (halfres_pending_rt_ == &sh.image) FlushHalfRes();
  if (sh.newer) {
    MsaaTransfer(rt, sh.image, false, sh.dirty_rows ? sh.dirty_rows * 2 : 0);
    sh.newer = false;
    sh.dirty_rows = 0;
  }
  if (invalidate) sh.valid = false;
}

void NativeCommandProcessor::ExportAllMsaaShadows() {
  for (auto& [k, rt] : render_targets_) ExportMsaaShadow(rt, false);
}

VkRenderPass NativeCommandProcessor::GetRenderPass(const VkFormat* color_formats, uint32_t color_count,
                                                   VkFormat depth_format, uint32_t samples) {
  uint64_t key = uint64_t(depth_format) | (uint64_t(color_count) << 10) | (uint64_t(samples) << 14);
  for (uint32_t i = 0; i < color_count; ++i) key = key * 1000003u + uint64_t(color_formats[i]);
  auto it = render_passes_.find(key);
  if (it != render_passes_.end()) return it->second;
  VkAttachmentDescription att[5] = {};
  VkAttachmentReference color_refs[4] = {};
  VkAttachmentReference depth_ref = {};
  uint32_t n = 0;
  for (uint32_t i = 0; i < color_count; ++i) {
    att[n].format = color_formats[i];
    att[n].samples = VkSampleCountFlagBits(samples);
    att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[n].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    att[n].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    color_refs[i] = {n, VK_IMAGE_LAYOUT_GENERAL};
    ++n;
  }
  if (depth_format != VK_FORMAT_UNDEFINED) {
    att[n].format = depth_format;
    att[n].samples = VkSampleCountFlagBits(samples);
    att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[n].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    att[n].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_ref = {n, VK_IMAGE_LAYOUT_GENERAL};
    ++n;
  }
  VkSubpassDescription sub = {};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = color_count;
  sub.pColorAttachments = color_refs;
  sub.pDepthStencilAttachment = depth_format != VK_FORMAT_UNDEFINED ? &depth_ref : nullptr;
  VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  ci.attachmentCount = n;
  ci.pAttachments = att;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  VkRenderPass pass = VK_NULL_HANDLE;
  if (dfn().vkCreateRenderPass(vulkan_device_->device(), &ci, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
  render_passes_.emplace(key, pass);
  return pass;
}

VkDescriptorSetLayout NativeCommandProcessor::GetTextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers) {
  if (!textures && !samplers) return set_layout_empty_;
  const uint32_t key = textures | (samplers << 12) | (uint32_t(vertex) << 24);
  auto it = texture_set_layouts_.find(key);
  if (it != texture_set_layouts_.end()) return it->second;
  std::vector<VkDescriptorSetLayoutBinding> b(textures + samplers);
  const VkShaderStageFlags stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < textures + samplers; ++i) {
    b[i].binding = i;
    b[i].descriptorType = i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    b[i].descriptorCount = 1;
    b[i].stageFlags = stage;
  }
  VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ci.bindingCount = uint32_t(b.size());
  ci.pBindings = b.data();
  VkDescriptorSetLayout l = VK_NULL_HANDLE;
  if (dfn().vkCreateDescriptorSetLayout(vulkan_device_->device(), &ci, nullptr, &l) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  texture_set_layouts_.emplace(key, l);
  return l;
}

const NativeCommandProcessor::PipelineLayoutEntry* NativeCommandProcessor::GetPipelineLayout(uint32_t tv, uint32_t sv,
                                                                                             uint32_t tp,
                                                                                             uint32_t sp) {
  const uint64_t key = uint64_t(tv) | (uint64_t(sv) << 16) | (uint64_t(tp) << 32) | (uint64_t(sp) << 48);
  auto it = pipeline_layouts_.find(key);
  if (it != pipeline_layouts_.end()) return &it->second;
  PipelineLayoutEntry e;
  e.textures_vertex = GetTextureSetLayout(true, tv, sv);
  e.textures_pixel = GetTextureSetLayout(false, tp, sp);
  if (!e.textures_vertex || !e.textures_pixel) return nullptr;
  VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = set_layout_shared_memory_;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = set_layout_constants_;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = e.textures_vertex;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = e.textures_pixel;
  VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ci.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  ci.pSetLayouts = sets;
  if (dfn().vkCreatePipelineLayout(vulkan_device_->device(), &ci, nullptr, &e.layout) != VK_SUCCESS) return nullptr;
  return &(pipeline_layouts_[key] = e);
}

VkPipeline NativeCommandProcessor::GetPipeline(const PipelineKey& key, VulkanShader::VulkanTranslation* vs,
                                               VulkanShader::VulkanTranslation* ps, VkPipelineLayout layout,
                                               bool allow_async, bool& pending_out) {
  pending_out = false;
  if (pipeline_done_count_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    for (const auto& [h, p] : pipeline_done_) {
      pipelines_[h] = p;
      pipeline_pending_.erase(h);
    }
    pipeline_done_.clear();
    pipeline_done_count_.store(0, std::memory_order_release);
  }
  // RC91 probe 3: the same key with the constant pixel shader is another pipeline (salted hash).
  const bool probe_ps = probe_active_ == 3 && ps && probe_fs_;
  const uint64_t hash = XXH3_64bits(&key, sizeof(key)) ^ (probe_ps ? 0x9E3779B97F4A7C15ull : 0ull);
  auto it = pipelines_.find(hash);
  if (it != pipelines_.end()) return it->second;
  const VkShaderModule vsm = vs->shader_module(),
                       psm = probe_ps ? probe_fs_ : (ps ? ps->shader_module() : VK_NULL_HANDLE);
  if (pipeline_pending_.count(hash)) {
    if (allow_async) {
      pending_out = true;
      return VK_NULL_HANDLE;
    }
    // Needed now (full-screen pass). RC77: if the worker has not started it yet, it is taken out of the queue and
    // created here instead of waiting behind every job queued before it (up to ~1 s on entering an area).
    ScopedUs timer(stats_.us_pipelines);
    ScopedUs sync_timer(stats_.us_pipeline_sync);
    ++stats_.pipelines_sync;
    std::unique_lock<std::mutex> lock(pipeline_mutex_);
    if (REXCVAR_GET(ds2_native_pipeline_steal)) {
      for (auto jit = pipeline_jobs_.begin(); jit != pipeline_jobs_.end(); ++jit) {
        if (jit->hash != hash) continue;
        const PipelineJob job = *jit;
        pipeline_jobs_.erase(jit);
        lock.unlock();
        VkPipeline pipeline = CreatePipelineObject(job.key, job.vs, job.ps, job.layout);
        ++stats_.pipelines;
        ++stats_.pipelines_stolen;
        pipelines_[hash] = pipeline;
        pipeline_pending_.erase(hash);
        return pipeline;
      }
    }
    for (;;) {
      for (const auto& [h, p] : pipeline_done_) {
        pipelines_[h] = p;
        pipeline_pending_.erase(h);
      }
      pipeline_done_.clear();
      pipeline_done_count_.store(0, std::memory_order_release);
      auto found = pipelines_.find(hash);
      if (found != pipelines_.end()) return found->second;
      pipeline_cv_.wait_for(lock, std::chrono::milliseconds(1));
    }
  }
  if (allow_async && REXCVAR_GET(ds2_native_async_pipelines)) {
    if (!pipeline_thread_started_) {
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 4 << 20);  // the shader compiler recurses deeply
      pipeline_thread_started_ = pthread_create(&pipeline_thread_, &attr, &PipelineThreadMain, this) == 0;
      pthread_attr_destroy(&attr);
    }
    if (pipeline_thread_started_) {
      {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        pipeline_jobs_.push_back({hash, key, vsm, psm, layout});
      }
      pipeline_cv_.notify_all();
      pipeline_pending_.insert(hash);
      ++stats_.pipelines_async;
      pending_out = true;
      return VK_NULL_HANDLE;
    }
  }
  ScopedUs timer(stats_.us_pipelines);
  ScopedUs sync_timer(stats_.us_pipeline_sync);
  ++stats_.pipelines_sync;
  VkPipeline pipeline = CreatePipelineObject(key, vsm, psm, layout);
  ++stats_.pipelines;
  pipelines_.emplace(hash, pipeline);
  return pipeline;
}

void* NativeCommandProcessor::PipelineThreadMain(void* param) {
  auto* self = static_cast<NativeCommandProcessor*>(param);
  for (;;) {
    PipelineJob job;
    {
      std::unique_lock<std::mutex> lock(self->pipeline_mutex_);
      self->pipeline_cv_.wait(lock, [self] { return self->pipeline_quit_ || !self->pipeline_jobs_.empty(); });
      if (self->pipeline_quit_) return nullptr;
      job = self->pipeline_jobs_.front();
      self->pipeline_jobs_.pop_front();
    }
    const uint64_t start = NowUs();
    VkPipeline p = self->CreatePipelineObject(job.key, job.vs, job.ps, job.layout);
    self->pipeline_async_us_.fetch_add(NowUs() - start, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(self->pipeline_mutex_);
      self->pipeline_done_.emplace_back(job.hash, p);
      self->pipeline_done_count_.store(uint32_t(self->pipeline_done_.size()), std::memory_order_release);
    }
    self->pipeline_cv_.notify_all();
  }
}

void* NativeCommandProcessor::RestoreThreadMain(void* param) {
  auto* self = static_cast<NativeCommandProcessor*>(param);
  for (;;) {
    Shader::Translation* job;
    {
      std::unique_lock<std::mutex> lock(self->restore_mutex_);
      self->restore_cv_.wait(lock, [self] { return self->restore_quit_ || !self->restore_jobs_.empty(); });
      if (self->restore_quit_) return nullptr;
      job = self->restore_jobs_.front();
      self->restore_jobs_.pop_front();
    }
    const uint64_t start = NowUs();
    const bool restored = RestoreConfiguredSpirvLibrary(*job, self->restore_signature_);
    self->restore_us_.fetch_add(NowUs() - start, std::memory_order_relaxed);
    self->restore_count_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(self->restore_mutex_);
      self->restore_done_.emplace_back(job, restored);
    }
    self->restore_cv_.notify_all();
  }
}

void NativeCommandProcessor::DrainRestores(bool wait_for_shader, const Shader* shader) {
  std::unique_lock<std::mutex> lock(restore_mutex_);
  for (;;) {
    for (const auto& [t, restored] : restore_done_) {
      restore_pending_.erase(t);
      auto it = restore_pending_shaders_.find(&t->shader());
      if (it != restore_pending_shaders_.end() && --it->second == 0) restore_pending_shaders_.erase(it);
      if (!restored) restore_failed_.insert(t);
    }
    restore_done_.clear();
    if (!wait_for_shader || !restore_pending_shaders_.count(shader)) return;
    restore_cv_.wait_for(lock, std::chrono::milliseconds(1));
  }
}

void NativeCommandProcessor::StopRestoreThread() {
  if (!restore_thread_started_) return;
  {
    std::lock_guard<std::mutex> lock(restore_mutex_);
    restore_quit_ = true;
    restore_jobs_.clear();
  }
  restore_cv_.notify_all();
  pthread_join(restore_thread_, nullptr);
  restore_thread_started_ = false;
}

bool NativeCommandProcessor::PrepareTranslation(VulkanShader::VulkanTranslation* t, bool allow_async) {
  if (!t) return true;
  if (restore_pending_.count(t)) {
    if (allow_async) return false;
    DrainRestores(true, &t->shader());
  }
  if (t->is_translated()) {
    if (t->is_valid() && !t->shader_module()) t->GetOrCreateShaderModule();
    return true;
  }
  const bool async = allow_async && REXCVAR_GET(ds2_native_async_shader_restore) && !restore_failed_.count(t) &&
                     !REXCVAR_GET(ds2_shader_library).empty();
  if (async) {
    if (!restore_thread_started_) {
      restore_signature_ = translator_->ShaderLibrarySignature();
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 1 << 20);
      restore_thread_started_ = pthread_create(&restore_thread_, &attr, &RestoreThreadMain, this) == 0;
      pthread_attr_destroy(&attr);
    }
    if (restore_thread_started_) {
      {
        std::lock_guard<std::mutex> lock(restore_mutex_);
        restore_jobs_.push_back(t);
      }
      restore_cv_.notify_all();
      restore_pending_.insert(t);
      ++restore_pending_shaders_[&t->shader()];
      ++stats_.shader_restores_async;
      return false;
    }
  }
  // At once (or the pack has no such variant): no restore of this shader may run on the worker meanwhile.
  if (restore_pending_shaders_.count(&t->shader())) DrainRestores(true, &t->shader());
  if (!translator_->TranslateAnalyzedShader(*t) || !t->GetOrCreateShaderModule()) {
    REXGPU_ERROR("[native] shader {:016X} translation failed", t->shader().ucode_data_hash());
  }
  return true;
}

void NativeCommandProcessor::StopPipelineThread() {
  if (!pipeline_thread_started_) return;
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipeline_quit_ = true;
    pipeline_jobs_.clear();
  }
  pipeline_cv_.notify_all();
  pthread_join(pipeline_thread_, nullptr);
  pipeline_thread_started_ = false;
  for (const auto& [h, p] : pipeline_done_) pipelines_[h] = p;
  pipeline_done_.clear();
}

VkPipeline NativeCommandProcessor::CreatePipelineObject(const PipelineKey& key, VkShaderModule vs_module,
                                                        VkShaderModule ps_module, VkPipelineLayout layout) {
  VkPipelineShaderStageCreateInfo stages[2] = {};
  uint32_t stage_count = 0;
  stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[stage_count].module = vs_module;
  stages[stage_count].pName = "main";
  ++stage_count;
  if (ps_module) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = ps_module;
    stages[stage_count].pName = "main";
    ++stage_count;
  }
  // RC96: texture signs (IDs kSpecTextureSignsBase + word); unused IDs in a module are ignored by Vulkan.
  // v107: + the gl_FragCoord scale of pixel shaders drawing into reduced-resolution targets.
  VkSpecializationMapEntry sign_entries[9];
  VkSpecializationInfo sign_info = {};
  struct {
    uint32_t signs[8];
    float frag_scale;
  } spec_data = {};
  static_assert(sizeof(key.texture_signs) == sizeof(spec_data.signs), "texture signs: 8 words");
  uint32_t spec_count = 0;
  if (REXCVAR_GET(ds2_native_fold_texture_signs)) {
    std::memcpy(spec_data.signs, key.texture_signs, sizeof(spec_data.signs));
    for (uint32_t w = 0; w < 8; ++w) {
      sign_entries[spec_count++] = {SpirvShaderTranslator::kSpecTextureSignsBase + w, uint32_t(w * sizeof(uint32_t)),
                                    sizeof(uint32_t)};
    }
  }
  if (key.samples_reserved && res_scale_ < 1.0f) {
    spec_data.frag_scale = 1.0f / res_scale_;
    sign_entries[spec_count++] = {vulkan::kFragCoordScaleSpecId, uint32_t(offsetof(decltype(spec_data), frag_scale)),
                                  sizeof(float)};
  }
  if (spec_count) {
    sign_info.mapEntryCount = spec_count;
    sign_info.pMapEntries = sign_entries;
    sign_info.dataSize = sizeof(spec_data);
    sign_info.pData = &spec_data;
    for (uint32_t i = 0; i < stage_count; ++i) stages[i].pSpecializationInfo = &sign_info;
  }
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = key.native_binding_count;
  vi.pVertexBindingDescriptions = key.native_bindings;
  vi.vertexAttributeDescriptionCount = key.native_attribute_count;
  vi.pVertexAttributeDescriptions = key.native_attributes;
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VkPrimitiveTopology(key.topology);
  ia.primitiveRestartEnable = key.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VkCullModeFlags(key.cull_mode);
  rs.frontFace = key.front_face_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.depthBiasEnable = VK_TRUE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  // RC71: Xenos 4x MSAA samples sit on an ordered 2x2 grid (the sample-space image has them as pixel centers).
  static const VkSampleLocationEXT kXenosLocations[4] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f},
                                                        {0.75f, 0.75f}};
  VkPipelineSampleLocationsStateCreateInfoEXT msl = {
      VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT};
  if (key.samples == 4) {
    ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
    msl.sampleLocationsEnable = VK_TRUE;
    msl.sampleLocationsInfo.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
    msl.sampleLocationsInfo.sampleLocationsPerPixel = VK_SAMPLE_COUNT_4_BIT;
    msl.sampleLocationsInfo.sampleLocationGridSize = {1, 1};
    msl.sampleLocationsInfo.sampleLocationsCount = 4;
    msl.sampleLocationsInfo.pSampleLocations = kXenosLocations;
    ms.pNext = &msl;
  }
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = VkCompareOp(key.depth_func);
  ds.stencilTestEnable = key.stencil_enable ? VK_TRUE : VK_FALSE;
  auto stencil = [](uint32_t packed) {
    VkStencilOpState s = {};
    s.failOp = VkStencilOp(packed & 7);
    s.passOp = VkStencilOp((packed >> 3) & 7);
    s.depthFailOp = VkStencilOp((packed >> 6) & 7);
    s.compareOp = VkCompareOp((packed >> 9) & 7);
    return s;
  };
  ds.front = stencil(key.stencil_front);
  ds.back = stencil(key.stencil_back);
  VkPipelineColorBlendAttachmentState blend[4] = {};
  for (uint32_t i = 0; i < key.color_count; ++i) {
    const uint32_t b = key.blend[i];
    VkPipelineColorBlendAttachmentState& a = blend[i];
    a.colorWriteMask = (key.write_mask >> (4 * i)) & 0xF;
    const uint32_t cs = b & 0x1F, cop = (b >> 5) & 7, cd = (b >> 8) & 0x1F;
    const uint32_t as = (b >> 16) & 0x1F, aop = (b >> 21) & 7, ad = (b >> 24) & 0x1F;
    a.blendEnable = (b == 0x00010001u) ? VK_FALSE : VK_TRUE;
    a.srcColorBlendFactor = kBlendFactors[cs];
    a.dstColorBlendFactor = kBlendFactors[cd];
    a.colorBlendOp = kBlendOps[cop];
    a.srcAlphaBlendFactor = kBlendFactors[as];
    a.dstAlphaBlendFactor = kBlendFactors[ad];
    a.alphaBlendOp = kBlendOps[aop];
  }
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = key.color_count;
  cb.pAttachments = blend;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                          VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                          VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                          VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = uint32_t(rex::countof(dyn));
  dy.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.stageCount = stage_count;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dy;
  ci.layout = layout;
  ci.renderPass = reinterpret_cast<VkRenderPass>(key.render_pass);
  ci.subpass = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn().vkCreateGraphicsPipelines(vulkan_device_->device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) !=
      VK_SUCCESS) {
    REXGPU_ERROR("[native] pipeline creation failed (vs {:016X} ps {:016X})", key.vs_hash, key.ps_hash);
    pipeline = VK_NULL_HANDLE;
  }
  return pipeline;
}

VkSampler NativeCommandProcessor::GetSampler(const VulkanShader::SamplerBinding& binding,
                                             const xenos::xe_gpu_texture_fetch_t& fetch) {
  auto filter = [](xenos::TextureFilter instr, xenos::TextureFilter from_fetch) {
    xenos::TextureFilter f = instr == xenos::TextureFilter::kUseFetchConst ? from_fetch : instr;
    return f == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  };
  const VkFilter mag = filter(binding.mag_filter, fetch.mag_filter);
  const VkFilter min = filter(binding.min_filter, fetch.min_filter);
  auto clamp = [](xenos::ClampMode c) {
    switch (c) {
      case xenos::ClampMode::kRepeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case xenos::ClampMode::kMirroredRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case xenos::ClampMode::kClampToBorder:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
  };
  const VkSamplerAddressMode u = clamp(fetch.clamp_x), v = clamp(fetch.clamp_y), w = clamp(fetch.clamp_z);
  // Mips (v54): mip filter, minimum level and anisotropy as in the emulation's sampler parameters.
  const xenos::TextureFilter mip_filter =
      binding.mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter : binding.mip_filter;
  const bool mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  const bool base_map = mip_filter == xenos::TextureFilter::kBaseMap || !REXCVAR_GET(ds2_native_mips);
  uint32_t mip_min_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr, &mip_min_level,
                                                 nullptr);
  if (!REXCVAR_GET(ds2_native_mips)) mip_min_level = 0;
  const xenos::AnisoFilter aniso_filter =
      binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter : binding.aniso_filter;
  uint32_t aniso = 1;
  if (aniso_filter != xenos::AnisoFilter::kDisabled && aniso_filter <= xenos::AnisoFilter::kMax_16_1) {
    aniso = 1u << (uint32_t(aniso_filter) - 1);
  }
  aniso = std::min<uint32_t>(aniso, uint32_t(std::max(REXCVAR_GET(ds2_native_max_anisotropy), 1)));
  if (!device()->properties().samplerAnisotropy || base_map) aniso = 1;
  aniso = std::min<uint32_t>(aniso, uint32_t(device()->properties().maxSamplerAnisotropy));
  const uint64_t key = uint64_t(mag) | (uint64_t(min) << 2) | (uint64_t(u) << 4) | (uint64_t(v) << 8) |
                       (uint64_t(w) << 12) | (uint64_t(mip_linear) << 16) | (uint64_t(base_map) << 17) |
                       (uint64_t(mip_min_level) << 18) | (uint64_t(aniso) << 24);
  auto it = samplers_.find(key);
  if (it != samplers_.end()) return it->second;
  VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  ci.magFilter = aniso > 1 ? VK_FILTER_LINEAR : mag;
  ci.minFilter = aniso > 1 ? VK_FILTER_LINEAR : min;
  ci.mipmapMode = (mip_linear || aniso > 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.addressModeU = u;
  ci.addressModeV = v;
  ci.addressModeW = w;
  ci.minLod = float(mip_min_level);
  ci.maxLod = base_map ? ci.minLod + 0.25f : VK_LOD_CLAMP_NONE;
  if (aniso > 1) {
    ci.anisotropyEnable = VK_TRUE;
    ci.maxAnisotropy = float(aniso);
  }
  ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler s = VK_NULL_HANDLE;
  dfn().vkCreateSampler(vulkan_device_->device(), &ci, nullptr, &s);
  samplers_.emplace(key, s);
  return s;
}

VkImageView NativeCommandProcessor::GetSwizzleView(std::unordered_map<uint32_t, VkImageView>& views,
                                                   const NativeImage& image, uint32_t swizzle) {
  auto it = views.find(swizzle);
  if (it != views.end()) return it->second;
  // swizzle: bits 0-11 guest swizzle, 12-23 host swizzle.
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = image.image;
  vci.viewType = image.volume_depth ? VK_IMAGE_VIEW_TYPE_3D
                 : image.cube       ? VK_IMAGE_VIEW_TYPE_CUBE
                                    : VK_IMAGE_VIEW_TYPE_2D;
  vci.format = image.format;
  const uint32_t guest = swizzle & 0xFFF, host = swizzle >> 12;
  vci.components.r = SwizzleComponent(guest, host, 0);
  vci.components.g = SwizzleComponent(guest, host, 1);
  vci.components.b = SwizzleComponent(guest, host, 2);
  vci.components.a = SwizzleComponent(guest, host, 3);
  vci.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
      image.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view = VK_NULL_HANDLE;
  if (dfn().vkCreateImageView(vulkan_device_->device(), &vci, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
  views.emplace(swizzle, view);
  return view;
}

void NativeCommandProcessor::DestroyResolveLogicalImages(ResolvedEntry& entry, bool deferred) {
  for (auto& [key, logical] : entry.logical_images) {
    for (auto& [swizzle, view] : logical.swizzle_views) {
      if (deferred) {
        slot().deferred_views.push_back(view);
      } else {
        DestroyView(view);
      }
    }
    DestroyImage(logical.image, deferred);
  }
  entry.logical_images.clear();
  for (auto& [key, decoded] : entry.depth_decoded) {
    for (auto& [swizzle, view] : decoded.swizzle_views) {
      if (deferred) {
        slot().deferred_views.push_back(view);
      } else {
        DestroyView(view);
      }
    }
    DestroyImage(decoded.image, deferred);
  }
  entry.depth_decoded.clear();
}

NativeCommandProcessor::ResolveLogicalImage* NativeCommandProcessor::GetResolveLogicalImage(
    ResolvedEntry& entry, uint32_t width, uint32_t height) {
  if (!REXCVAR_GET(ds2_native_resolve_logical_size)) return nullptr;
  // Preserve sample-space MSAA (not downsampling): only discard layout padding.
  // Check in 64 bits before narrowing, including a defensive zero-size check.
  // v107: and the reduced internal resolution of the source.
  const uint64_t scaled_width = uint64_t(std::lround(double(width) * entry.sample_scale_x * entry.scale));
  const uint64_t scaled_height = uint64_t(std::lround(double(height) * entry.sample_scale_y * entry.scale));
  if (!scaled_width || !scaled_height || scaled_width > entry.image.width ||
      scaled_height > entry.image.height ||
      (scaled_width == entry.image.width && scaled_height == entry.image.height)) {
    return nullptr;
  }
  width = uint32_t(scaled_width);
  height = uint32_t(scaled_height);
  const uint64_t key = uint64_t(width) | (uint64_t(height) << 32);
  ResolveLogicalImage& logical = entry.logical_images[key];
  if (!logical.image.image) {
    if (!CreateImage(logical.image, entry.image.format, width, height, entry.image.depth,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      // CreateImage may have allocated the image before a view allocation failed.
      // Nothing using this new image has been recorded, so immediate cleanup is safe.
      DestroyImage(logical.image, false);
      entry.logical_images.erase(key);
      return nullptr;
    }
    EnsureGeneral(logical.image);
  }
  // A resolve may be updated and sampled multiple times in one frame. A frame tag would
  // incorrectly reuse the first copy; every successful root/sub-resolve bumps generation.
  if (logical.generation != entry.generation) {
    EndRenderPass();
    ResolveBarrier(entry.image, logical.image, true);
    VkImageCopy copy = {};
    copy.srcSubresource = {VkImageAspectFlags(entry.image.depth
                              ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                              : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {width, height, 1};
    work_since_barrier_ = true;
    cmd_copy_image_(slot().cmd, entry.image.image, VK_IMAGE_LAYOUT_GENERAL, logical.image.image,
                    VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ResolveBarrier(entry.image, logical.image, false);
    logical.generation = entry.generation;
    if (Tracing()) {
      REXGPU_INFO("[native trace]   resolve logical image {}x{} from {}x{} generation {}", width, height,
                  entry.image.width, entry.image.height, entry.generation);
    }
  }
  return &logical;
}

VkImageView NativeCommandProcessor::GetTextureView(uint32_t fetch_index, xenos::FetchOpDimension dimension,
                                                   bool& resolved_out) {
  resolved_out = false;
  const RegisterFile& regs = *register_file_;
  const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(fetch_index);
  // RC63: 1D textures (the translator binds tfetch1D as a 2D array, so dimension is k2D here) are a 2D image one
  // row tall. Before RC63 every 1D fetch read the white 1x1 texture (e.g. DS2's 256-entry tone curve).
  const bool is_1d = fetch.dimension == xenos::DataDimension::k1D && REXCVAR_GET(ds2_native_textures_1d);
  if (dimension == xenos::FetchOpDimension::kCube) {
    // RC64: cube maps (DS2 materials: specular/ambient cube tf9 with register LOD). Before RC64 the 2D white view
    // was bound to a cube binding: undefined on NVK (dark or unstable lighting).
    if (fetch.type == xenos::FetchConstantType::kTexture && fetch.dimension == xenos::DataDimension::kCube &&
        REXCVAR_GET(ds2_native_textures_cube) && REXCVAR_GET(ds2_native_textures)) {
      if (VkImageView v = GetCubeTextureView(fetch)) {
        ++stats_.texture_cube;
        return v;
      }
    }
    ++stats_.texture_white_dimension;
    return white_cube_.view ? white_cube_.view : white_view_;
  }
  if (dimension == xenos::FetchOpDimension::k3DOrStacked) {
    // v104: 3D textures (DS2 DLC areas: the color grading volume of the tone mapping PS 989C5498). Before v104 the
    // 2D white view was bound to a 3D binding: undefined on NVK, the DLC picture was black under the HUD.
    if (fetch.type == xenos::FetchConstantType::kTexture && fetch.dimension == xenos::DataDimension::k3D &&
        REXCVAR_GET(ds2_native_textures)) {
      if (VkImageView v = Get3DTextureView(fetch)) {
        ++stats_.texture_3d;
        return v;
      }
    }
    if (fetch.type == xenos::FetchConstantType::kTexture && fetch.dimension == xenos::DataDimension::k3D)
      ++stats_.texture_white_dimension;
    return white_3d_.view ? white_3d_.view : white_view_;
  }
  if (fetch.type != xenos::FetchConstantType::kTexture || dimension != xenos::FetchOpDimension::k2D ||
      (fetch.dimension != xenos::DataDimension::k2DOrStacked && !is_1d)) {
    if (fetch.type == xenos::FetchConstantType::kTexture) ++stats_.texture_white_dimension;
    return white_view_;
  }
  const uint32_t base = uint32_t(fetch.base_address) << 12;
  const uint32_t width = is_1d ? fetch.size_1d.width + 1 : fetch.size_2d.width + 1;
  const uint32_t height = is_1d ? 1u : fetch.size_2d.height + 1;
  if (is_1d) ++stats_.texture_1d;
  TextureFormatInfo fi;
  const bool format_known = GetTextureFormat(fetch.format, host_bcn_, fi);
  // A resolve target at this address: sample the native image directly.
  auto rit = resolved_.find(base);
  if (rit != resolved_.end()) {
    resolved_out = true;
    rit->second.frame = frame_;
    if (Tracing()) {
      REXGPU_INFO("[native trace]   draw {} samples resolve {:08X} as fmt {} {}x{} endian {} swizzle {:03X} signs "
                  "{}{}{}{} exp {}",
                  draws_this_frame_, base, uint32_t(fetch.format), width, height, uint32_t(fetch.endianness),
                  uint32_t(fetch.swizzle), uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                  uint32_t(fetch.sign_w), int32_t(fetch.exp_adjust));
    }
    ++stats_.texture_from_resolve;
    // The guest copy had red and blue swapped when copy_dest_swap was set; the native copy did not.
    // Depth images have one component: every guest component reads it (k_24_8 / k_24_8_FLOAT are RRRR in the
    // emulation too).
    uint32_t host = rit->second.image.depth ? uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR)
                    : !format_known         ? uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA)
                                            : fi.host_swizzle;
    if (rit->second.rb_swapped && !rit->second.image.depth) {
      const uint32_t h[4] = {host & 7, (host >> 3) & 7, (host >> 6) & 7, (host >> 9) & 7};
      host = h[2] | (h[1] << 3) | (h[0] << 6) | (h[3] << 9);
    }
    // RC64: k_24_8 / k_24_8_FLOAT sampled from a depth word the game resolved into an 8888 surface (shadow
    // maps 1024/1600, scene depth 1280x720/640x360 for fog, DOF and soft particles). Before RC64 the shader read
    // the raw bytes (R = stencil): shadows let light through walls, depth effects drew halos.
    if ((fetch.format == xenos::TextureFormat::k_24_8 || fetch.format == xenos::TextureFormat::k_24_8_FLOAT) &&
        !rit->second.image.depth && rit->second.image.format == VK_FORMAT_R8G8B8A8_UNORM &&
        REXCVAR_GET(ds2_native_depth_from_rgba8)) {
      if (VkImageView v = GetResolveDepthView(rit->second, width, height,
                                              fetch.format == xenos::TextureFormat::k_24_8_FLOAT, fetch.swizzle)) {
        ++stats_.depth_decoded_fetches;
        return v;
      }
    }
    if (ResolveLogicalImage* logical = GetResolveLogicalImage(rit->second, width, height)) {
      return GetSwizzleView(logical->swizzle_views, logical->image, fetch.swizzle | (host << 12));
    }
    return GetSwizzleView(rit->second.swizzle_views, rit->second.image, fetch.swizzle | (host << 12));
  }
  if (!REXCVAR_GET(ds2_native_textures)) return white_view_;
  if (!format_known) {
    ++stats_.texture_unsupported;
    return white_view_;
  }
  // Mip levels (v54): level 0 from base_address, levels 1..mip_max_level from mip_address (packed mip tail as in
  // texture_util / the emulation's texture loader).
  uint32_t base_page = 0, mip_page = 0, mip_min_level = 0, mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page, &mip_page,
                                                 &mip_min_level, &mip_max_level);
  if (!REXCVAR_GET(ds2_native_mips) || !mip_page) mip_max_level = 0;
  const uint32_t levels = mip_max_level + 1;
  const uint32_t key_words[5] = {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_5, levels};
  const uint64_t key = XXH3_64bits(key_words, sizeof(key_words)) ^ (uint64_t(fetch.endianness) << 60);
  TextureEntry& t = textures_[key];
  t.last_use_frame = frame_;
  // Guest layout of the base level.
  const uint32_t bw = (width + fi.block_size - 1) / fi.block_size;
  const uint32_t bh = (height + fi.block_size - 1) / fi.block_size;
  const uint32_t pitch_blocks = (uint32_t(fetch.pitch) << 5) / fi.block_size;
  const uint32_t pitch_aligned = rex::align<uint32_t>(std::max(pitch_blocks, bw), 32);
  const uint32_t height_aligned = rex::align<uint32_t>(bh, 32);
  uint32_t off_x = 0, off_y = 0, off_z = 0;
  if (fetch.packed_mips) {
    texture_util::GetPackedMipOffset(width, height, 1, fetch.format, 0, off_x, off_y, off_z);
  }
  const uint32_t bpb_log2 = rex::log2_floor(fi.bytes_per_block);
  const size_t guest_size = size_t(pitch_aligned) * height_aligned * fi.bytes_per_block;
  const uint8_t* guest = memory_->TranslatePhysical<const uint8_t*>(base);
  if (!guest) return white_view_;
  const bool need_check = !t.image.image || frame_ - t.last_check_frame >= 30 ||
                          (frame_ < t.dynamic_until && frame_ != t.last_check_frame);
  if (!need_check) {
    return GetSwizzleView(t.swizzle_views, t.image, fetch.swizzle | (fi.host_swizzle << 12));
  }
  ScopedUs texture_timer(stats_.us_textures);
  // RC77: textures created together (entering an area) are rehashed on different frames, not all every 30th.
  t.last_check_frame = frame_ - (t.image.image ? 0 : std::min<uint64_t>(frame_, key % 29));
  texture_util::TextureGuestLayout layout = {};
  const uint8_t* guest_mips = nullptr;
  if (levels > 1) {
    layout = texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1,
                                                 fetch.tiled, fetch.format, fetch.packed_mips, true, levels - 1);
    guest_mips = memory_->TranslatePhysical<const uint8_t*>(mip_page << 12);
    if (!guest_mips || !layout.mips_total_extent_bytes) return white_view_;
  }
  uint64_t content = XXH3_64bits(guest, std::min<size_t>(guest_size, 64 << 20));
  if (guest_mips) {
    content ^= XXH3_64bits(guest_mips, std::min<size_t>(layout.mips_total_extent_bytes, 64 << 20)) * 31;
  }
  if (t.image.image && content == t.content_hash) {
    return GetSwizzleView(t.swizzle_views, t.image, fetch.swizzle | (fi.host_swizzle << 12));
  }
  if (!t.image.image) {
    if (!CreateImage(t.image, fi.format, width, height, false,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, levels)) {
      textures_.erase(key);
      return white_view_;
    }
  }
  if (t.image.image && REXCVAR_GET(ds2_native_dynamic_textures)) {
    // RC86: the game rewrote this texture (a video frame, a texture streamed in place): check it every frame for a
    // while instead of every 30 frames (the intro video stuttered and showed old frames).
    t.dynamic_until = frame_ + 120;
    ++stats_.dynamic_texture_updates;
  }
  t.content_hash = content;
  // Untile / copy every level into the upload buffer, swapping the byte order. Hosts without BCn:
  // the untiled guest blocks are staged in a scratch buffer and then CPU-decoded into the padded
  // whole-block rows of the upload buffer (the copy extent crops the row padding).
  const bool decoded = fi.decoded_texel_bytes != 0;
  VkDeviceSize level_offsets[xenos::kTextureMaxMips + 1];
  VkDeviceSize total = 0;
  size_t guest_blocks_max = size_t(bw) * bh * fi.bytes_per_block;
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t lbw = (std::max(width >> l, 1u) + fi.block_size - 1) / fi.block_size;
    const uint32_t lbh = (std::max(height >> l, 1u) + fi.block_size - 1) / fi.block_size;
    level_offsets[l] = total;
    total += rex::align<VkDeviceSize>(VkDeviceSize(lbw) * lbh * fi.host_block_bytes(), 16);
    if (l) {
      guest_blocks_max = std::max(guest_blocks_max, size_t(lbw) * lbh * fi.bytes_per_block);
    }
  }
  VkDeviceSize offset;
  uint8_t* dst_all = Upload(total, 16, offset);
  if (!dst_all) return white_view_;
  if (decoded && texture_block_scratch_.size() < guest_blocks_max) {
    texture_block_scratch_.resize(guest_blocks_max);
  }
  // The guest block staging destination (scratch when decoding, the upload buffer directly when
  // the compressed data is uploaded as-is).
  auto level_dst = [&](uint32_t l) -> uint8_t* {
    return decoded ? texture_block_scratch_.data() : dst_all + level_offsets[l];
  };
  // One level: blocks (x + ox, y + oy) of a guest image with the given pitch (blocks for tiled, bytes per row
  // for linear), bounded by src_size bytes.
  auto copy_level = [&](uint8_t* dst, uint32_t lbw, uint32_t lbh, const uint8_t* src, size_t src_size,
                        uint32_t tiled_pitch_blocks, size_t linear_row_bytes, uint32_t ox, uint32_t oy) {
    const VkDeviceSize row_bytes = VkDeviceSize(lbw) * fi.bytes_per_block;
    // RC80: rows are built and byte-swapped in cached memory and then written once to the upload buffer, which is
    // uncached (the in-place swap read it back: ~110 MB/s for textures when entering an area).
    const bool scratch = REXCVAR_GET(ds2_native_texture_scratch);
    if (scratch && texture_row_scratch_.size() < row_bytes) texture_row_scratch_.resize(size_t(row_bytes));
    for (uint32_t y = 0; y < lbh; ++y) {
      uint8_t* const out_row = dst + row_bytes * y;
      uint8_t* row = scratch ? texture_row_scratch_.data() : out_row;
      if (fetch.tiled && REXCVAR_GET(ds2_native_fast_untile)) {
        // RC77: the tiled layout keeps 16 bytes of a row contiguous (4 blocks of 4 bytes, 2 of 8, 1 of 16), so
        // the offset is computed once per 16 bytes, inline (GetTiledOffset2D, same arithmetic).
        const uint32_t run = bpb_log2 ? std::max<uint32_t>(16u >> bpb_log2, 1u) : 1u;
        const uint32_t run_bytes = run * fi.bytes_per_block;
        const int32_t yy = int32_t(y + oy);
        const int32_t pitch32 = int32_t(rex::align<uint32_t>(tiled_pitch_blocks, 32) >> 5);
        auto tiled = [&](int32_t xx) -> int32_t {
          int32_t macro = ((xx >> 5) + (yy >> 5) * pitch32) << (bpb_log2 + 7);
          int32_t micro = ((xx & 7) + ((yy & 0xE) << 2)) << bpb_log2;
          int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((yy & 1) << 4);
          return ((offset & ~0x1FF) << 3) + ((yy & 16) << 7) + ((offset & 0x1C0) << 2) +
                 (((((yy & 8) >> 2) + (xx >> 3)) & 3) << 6) + (offset & 0x3F);
        };
        uint32_t x = 0;
        while (x < lbw) {
          const int32_t xx = int32_t(x + ox);
          const int32_t o = tiled(xx);
          uint8_t* d = row + size_t(x) * fi.bytes_per_block;
          if ((uint32_t(xx) % run) == 0 && x + run <= lbw && o >= 0 && size_t(o) + run_bytes <= src_size) {
            std::memcpy(d, src + o, run_bytes);
            x += run;
            continue;
          }
          if (o < 0 || size_t(o) + fi.bytes_per_block > src_size) {
            std::memset(d, 0, fi.bytes_per_block);
          } else {
            std::memcpy(d, src + o, fi.bytes_per_block);
          }
          ++x;
        }
      } else if (fetch.tiled) {
        for (uint32_t x = 0; x < lbw; ++x) {
          const int32_t o = texture_util::GetTiledOffset2D(int32_t(x + ox), int32_t(y + oy), tiled_pitch_blocks,
                                                           bpb_log2);
          if (o < 0 || size_t(o) + fi.bytes_per_block > src_size) {
            std::memset(row + size_t(x) * fi.bytes_per_block, 0, fi.bytes_per_block);
            continue;
          }
          std::memcpy(row + size_t(x) * fi.bytes_per_block, src + o, fi.bytes_per_block);
        }
      } else {
        const size_t so = size_t(y + oy) * linear_row_bytes + size_t(ox) * fi.bytes_per_block;
        if (so + row_bytes <= src_size) {
          std::memcpy(row, src + so, row_bytes);
        } else {
          std::memset(row, 0, row_bytes);
        }
      }
      SwapElements(row, row_bytes, fetch.endianness);
      if (row != out_row) std::memcpy(out_row, row, size_t(row_bytes));
    }
  };
  copy_level(level_dst(0), bw, bh, guest, guest_size, pitch_aligned, size_t(pitch_aligned) * fi.bytes_per_block,
             off_x, off_y);
  if (decoded) {
    DecodeBcnBlocks(fetch.format, fi.bytes_per_block, fi.decoded_texel_bytes, texture_block_scratch_.data(), bw, bh,
                    dst_all);
  }
  for (uint32_t l = 1; l < levels; ++l) {
    const uint32_t lbw = (std::max(width >> l, 1u) + fi.block_size - 1) / fi.block_size;
    const uint32_t lbh = (std::max(height >> l, 1u) + fi.block_size - 1) / fi.block_size;
    const uint32_t stored = std::min(l, layout.packed_level);
    const auto& ll = layout.mips[stored];
    uint32_t ox = 0, oy = 0, oz = 0;
    if (l >= layout.packed_level) {
      texture_util::GetPackedMipOffset(width, height, 1, fetch.format, l, ox, oy, oz);
    }
    const uint32_t mo = layout.mip_offsets_bytes[stored];
    if (mo >= layout.mips_total_extent_bytes) {
      std::memset(dst_all + level_offsets[l], 0, size_t(lbw) * lbh * fi.host_block_bytes());
      continue;
    }
    copy_level(level_dst(l), lbw, lbh, guest_mips + mo, layout.mips_total_extent_bytes - mo,
               ll.row_pitch_bytes >> bpb_log2, ll.row_pitch_bytes, ox, oy);
    if (decoded) {
      DecodeBcnBlocks(fetch.format, fi.bytes_per_block, fi.decoded_texel_bytes, texture_block_scratch_.data(), lbw,
                      lbh, dst_all + level_offsets[l]);
    }
  }
  EndRenderPass();
  EnsureGeneral(t.image);
  Barrier();
  if (profiling_) Mark("texture upload");
  VkBufferImageCopy regions[xenos::kTextureMaxMips + 1] = {};
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t lw = std::max(width >> l, 1u), lh = std::max(height >> l, 1u);
    VkBufferImageCopy& r = regions[l];
    r.bufferOffset = offset + level_offsets[l];
    r.bufferRowLength = (lw + fi.block_size - 1) / fi.block_size * fi.block_size;
    r.bufferImageHeight = (lh + fi.block_size - 1) / fi.block_size * fi.block_size;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
    r.imageExtent = {lw, lh, 1};
  }
  work_since_barrier_ = true;
  dfn().vkCmdCopyBufferToImage(slot().cmd, slot().upload, t.image.image, VK_IMAGE_LAYOUT_GENERAL, levels, regions);
  Barrier();
  ++stats_.texture_uploads;
  stats_.texture_levels += levels;
  stats_.texture_upload_bytes += total;
  return GetSwizzleView(t.swizzle_views, t.image, fetch.swizzle | (fi.host_swizzle << 12));
}

VkImageView NativeCommandProcessor::GetResolveDepthView(ResolvedEntry& entry, uint32_t width, uint32_t height,
                                                       bool float24, uint32_t guest_swizzle) {
  if (!convert_fs_[kConvertRgba8ToDepth]) return VK_NULL_HANDLE;
  // Logical (fetch) size in the resolve's sample space, clamped to the image.
  const uint32_t w = std::min<uint32_t>(uint32_t(std::lround(double(width) * entry.sample_scale_x * entry.scale)),
                                        entry.image.width);
  const uint32_t h = std::min<uint32_t>(uint32_t(std::lround(double(height) * entry.sample_scale_y * entry.scale)),
                                        entry.image.height);
  if (!w || !h) return VK_NULL_HANDLE;
  const uint64_t key = uint64_t(w) | (uint64_t(h) << 24) | (uint64_t(float24) << 48);
  ResolveLogicalImage& d = entry.depth_decoded[key];
  if (!d.image.image) {
    if (!CreateImage(d.image, VK_FORMAT_R32_SFLOAT, w, h, false,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
      DestroyImage(d.image, false);
      entry.depth_decoded.erase(key);
      return VK_NULL_HANDLE;
    }
    EnsureGeneral(d.image);
  }
  if (d.generation != entry.generation) {
    if (!Convert(kConvertRgba8ToDepth, entry.image, d.image, 0, 0, {{0, 0}, {w, h}}, float24)) {
      return VK_NULL_HANDLE;
    }
    d.generation = entry.generation;
    ++stats_.depth_decodes;
  }
  return GetSwizzleView(d.swizzle_views, d.image, guest_swizzle | (uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR) << 12));
}

// RC64: a guest cube texture (6 faces as array slices, tiled or linear, packed mips per face) as a Vulkan cube.
// Faces the game rendered and resolved this session (native images, not in guest memory) are copied from the
// resolve targets at the face addresses; the others come from guest memory.
// v104: a 3D (volume) texture, base level only (DS2: the color grading volume of the DLC tone mapping).
VkImageView NativeCommandProcessor::Get3DTextureView(const xenos::xe_gpu_texture_fetch_t& fetch) {
  TextureFormatInfo fi;
  if (!GetTextureFormat(fetch.format, host_bcn_, fi)) {
    ++stats_.texture_unsupported;
    return VK_NULL_HANDLE;
  }
  const uint32_t base = uint32_t(fetch.base_address) << 12;
  const uint32_t width = fetch.size_3d.width + 1, height = fetch.size_3d.height + 1, depth = fetch.size_3d.depth + 1;
  const uint32_t key_words[5] = {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_5, 0x3D3Du};
  const uint64_t key = XXH3_64bits(key_words, sizeof(key_words)) ^ (uint64_t(fetch.endianness) << 60);
  TextureEntry& t = textures_[key];
  t.last_use_frame = frame_;
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k3D, fetch.pitch, width, height, depth, fetch.tiled, fetch.format, fetch.packed_mips,
      true, 0);
  const uint8_t* guest = memory_->TranslatePhysical<const uint8_t*>(base);
  const size_t extent = layout.base.level_data_extent_bytes;
  if (!guest || !extent || !layout.base.row_pitch_bytes) return VK_NULL_HANDLE;
  const uint32_t swz = fetch.swizzle | (fi.host_swizzle << 12);
  const bool need_check = !t.image.image || frame_ - t.last_check_frame >= 30 ||
                          (frame_ < t.dynamic_until && frame_ != t.last_check_frame);
  if (!need_check) return GetSwizzleView(t.swizzle_views, t.image, swz);
  ScopedUs texture_timer(stats_.us_textures);
  t.last_check_frame = frame_;
  const uint64_t content = XXH3_64bits(guest, std::min<size_t>(extent, 64 << 20));
  if (t.image.image && content == t.content_hash) return GetSwizzleView(t.swizzle_views, t.image, swz);
  if (!t.image.image) {
    if (!CreateImage(t.image, fi.format, width, height, false,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, false, 1, depth)) {
      DestroyImage(t.image, false);
      textures_.erase(key);
      return VK_NULL_HANDLE;
    }
  } else if (REXCVAR_GET(ds2_native_dynamic_textures)) {
    t.dynamic_until = frame_ + 120;
    ++stats_.dynamic_texture_updates;
  }
  t.content_hash = content;
  const uint32_t bpb_log2 = rex::log2_floor(fi.bytes_per_block);
  const uint32_t bw = (width + fi.block_size - 1) / fi.block_size;
  const uint32_t bh = (height + fi.block_size - 1) / fi.block_size;
  // Hosts without BCn: untile the guest blocks into a scratch buffer, then CPU-decode into the
  // upload buffer.
  const bool decoded = fi.decoded_texel_bytes != 0;
  const size_t guest_row_bytes = size_t(bw) * fi.bytes_per_block;
  const size_t row_bytes = size_t(bw) * fi.host_block_bytes();
  const VkDeviceSize total = VkDeviceSize(row_bytes) * bh * depth;
  VkDeviceSize offset;
  uint8_t* dst = Upload(total, 16, offset);
  if (!dst) return VK_NULL_HANDLE;
  uint8_t* block_dst = dst;
  if (decoded) {
    if (texture_block_scratch_.size() < guest_row_bytes * size_t(bh) * depth) {
      texture_block_scratch_.resize(guest_row_bytes * size_t(bh) * depth);
    }
    block_dst = texture_block_scratch_.data();
  }
  const uint32_t pitch_blocks = layout.base.row_pitch_bytes >> bpb_log2;
  const uint32_t slice_rows = layout.base.z_slice_stride_block_rows;
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < bh; ++y) {
      uint8_t* row = block_dst + (size_t(z) * bh + y) * guest_row_bytes;
      if (fetch.tiled) {
        for (uint32_t x = 0; x < bw; ++x) {
          const int32_t o = texture_util::GetTiledOffset3D(int32_t(x), int32_t(y), int32_t(z), pitch_blocks,
                                                           slice_rows, bpb_log2);
          if (o < 0 || size_t(o) + fi.bytes_per_block > extent) {
            std::memset(row + size_t(x) * fi.bytes_per_block, 0, fi.bytes_per_block);
            continue;
          }
          std::memcpy(row + size_t(x) * fi.bytes_per_block, guest + o, fi.bytes_per_block);
        }
      } else {
        const size_t so = (size_t(z) * slice_rows + y) * layout.base.row_pitch_bytes;
        if (so + guest_row_bytes <= extent) {
          std::memcpy(row, guest + so, guest_row_bytes);
        } else {
          std::memset(row, 0, guest_row_bytes);
        }
      }
      SwapElements(row, guest_row_bytes, fetch.endianness);
    }
  }
  if (decoded) {
    for (uint32_t z = 0; z < depth; ++z) {
      DecodeBcnBlocks(fetch.format, fi.bytes_per_block, fi.decoded_texel_bytes,
                      texture_block_scratch_.data() + size_t(z) * bh * guest_row_bytes, bw, bh,
                      dst + size_t(z) * bh * row_bytes);
    }
  }
  EndRenderPass();
  EnsureGeneral(t.image);
  Barrier();
  if (profiling_) Mark("texture upload 3d");
  VkBufferImageCopy r = {};
  r.bufferOffset = offset;
  r.bufferRowLength = bw * fi.block_size;
  r.bufferImageHeight = bh * fi.block_size;
  r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.imageExtent = {width, height, depth};
  work_since_barrier_ = true;
  dfn().vkCmdCopyBufferToImage(slot().cmd, slot().upload, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
  Barrier();
  ++stats_.texture_uploads;
  stats_.texture_levels += 1;
  stats_.texture_upload_bytes += total;
  static uint32_t logged_3d = 0;
  if (logged_3d < 16 && ++logged_3d)
    REXGPU_INFO("[native] 3D texture {:08X} fmt {} {}x{}x{} tiled {} pitch {} endian {} uploaded",
              base, uint32_t(fetch.format), width, height, depth, uint32_t(fetch.tiled), uint32_t(fetch.pitch),
              uint32_t(fetch.endianness));
  return GetSwizzleView(t.swizzle_views, t.image, swz);
}

VkImageView NativeCommandProcessor::GetCubeTextureView(const xenos::xe_gpu_texture_fetch_t& fetch) {
  TextureFormatInfo fi;
  if (!GetTextureFormat(fetch.format, host_bcn_, fi)) {
    ++stats_.texture_unsupported;
    return VK_NULL_HANDLE;
  }
  const uint32_t base = uint32_t(fetch.base_address) << 12;
  const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
  uint32_t base_page = 0, mip_page = 0, mip_min_level = 0, mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page, &mip_page,
                                                 &mip_min_level, &mip_max_level);
  if (!REXCVAR_GET(ds2_native_mips) || !mip_page) mip_max_level = 0;
  const uint32_t levels = mip_max_level + 1;
  const uint32_t key_words[6] = {fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_5, levels, 0xC0BEu};
  const uint64_t key = XXH3_64bits(key_words, sizeof(key_words)) ^ (uint64_t(fetch.endianness) << 60);
  TextureEntry& t = textures_[key];
  t.last_use_frame = frame_;
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::kCube, fetch.pitch, width, height, 6, fetch.tiled, fetch.format, fetch.packed_mips, true,
      levels - 1);
  const uint32_t face_stride = layout.base.array_slice_stride_bytes;
  const uint8_t* guest = memory_->TranslatePhysical<const uint8_t*>(base);
  if (!guest || !face_stride) return VK_NULL_HANDLE;
  const uint8_t* guest_mips = nullptr;
  if (levels > 1) {
    guest_mips = memory_->TranslatePhysical<const uint8_t*>(mip_page << 12);
    if (!guest_mips || !layout.mips_total_extent_bytes) return VK_NULL_HANDLE;
  }
  // Faces (any level, not inside a packed mip tail) the game resolved: same format, at least the level size.
  // DS2 renders its environment cubes this way (64x64 faces 0x4000 apart, mips 32/16/8 resolved separately).
  ResolvedEntry* face_resolves[xenos::kTextureMaxMips + 1][6] = {};
  uint64_t resolve_signature = 0;
  for (uint32_t l = 0; l < levels; ++l) {
    if (fetch.packed_mips && l >= layout.packed_level) break;
    const uint32_t stored = l == 0 ? 0 : std::min(l, layout.packed_level);
    const auto& ll = l == 0 ? layout.base : layout.mips[stored];
    const uint32_t level_base = l == 0 ? base : (mip_page << 12) + layout.mip_offsets_bytes[stored];
    const uint32_t lw = std::max(width >> l, 1u), lh = std::max(height >> l, 1u);
    for (uint32_t f = 0; f < 6; ++f) {
      auto rit = resolved_.find(level_base + f * ll.array_slice_stride_bytes);
      if (rit == resolved_.end() || rit->second.image.depth || rit->second.image.format != fi.format ||
          rit->second.image.width < lw || rit->second.image.height < lh) {
        continue;
      }
      face_resolves[l][f] = &rit->second;
      rit->second.frame = frame_;
      resolve_signature = resolve_signature * 1000003u + rit->second.generation + 1 + f + 7 * l;
    }
  }
  const uint32_t swz = fetch.swizzle | (fi.host_swizzle << 12);
  const bool need_check = !t.image.image || frame_ - t.last_check_frame >= 30 ||
                          resolve_signature != t.resolve_signature ||
                          (frame_ < t.dynamic_until && frame_ != t.last_check_frame);
  if (!need_check) return GetSwizzleView(t.swizzle_views, t.image, swz);
  ScopedUs texture_timer(stats_.us_textures);
  t.last_check_frame = frame_;
  const size_t base_extent = size_t(face_stride) * 6;
  uint64_t content = XXH3_64bits(guest, std::min<size_t>(base_extent, 64 << 20)) ^ resolve_signature * 0x9E3779B97F4A7C15ull;
  if (guest_mips) content ^= XXH3_64bits(guest_mips, std::min<size_t>(layout.mips_total_extent_bytes, 64 << 20)) * 31;
  if (t.image.image && content == t.content_hash) {
    t.resolve_signature = resolve_signature;
    return GetSwizzleView(t.swizzle_views, t.image, swz);
  }
  if (!t.image.image) {
    if (!CreateImage(t.image, fi.format, width, height, false,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, levels, true)) {
      DestroyImage(t.image, false);
      textures_.erase(key);
      return VK_NULL_HANDLE;
    }
  }
  if (t.image.image && REXCVAR_GET(ds2_native_dynamic_textures)) {
    // RC86: the game rewrote this texture (a video frame, a texture streamed in place): check it every frame for a
    // while instead of every 30 frames (the intro video stuttered and showed old frames).
    t.dynamic_until = frame_ + 120;
    ++stats_.dynamic_texture_updates;
  }
  t.content_hash = content;
  t.resolve_signature = resolve_signature;
  const uint32_t bpb_log2 = rex::log2_floor(fi.bytes_per_block);
  // Hosts without BCn: guest blocks are staged per face in a scratch buffer (the base level is the
  // largest one), then CPU-decoded into the upload buffer.
  const bool decoded = fi.decoded_texel_bytes != 0;
  if (decoded) {
    const size_t base_guest_blocks = size_t((width + fi.block_size - 1) / fi.block_size) *
                                     ((height + fi.block_size - 1) / fi.block_size) * fi.bytes_per_block;
    if (texture_block_scratch_.size() < base_guest_blocks) texture_block_scratch_.resize(base_guest_blocks);
  }
  // Upload buffer: level-major, then face.
  VkDeviceSize face_offsets[xenos::kTextureMaxMips + 1][6];
  VkDeviceSize total = 0;
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t lbw = (std::max(width >> l, 1u) + fi.block_size - 1) / fi.block_size;
    const uint32_t lbh = (std::max(height >> l, 1u) + fi.block_size - 1) / fi.block_size;
    for (uint32_t f = 0; f < 6; ++f) {
      face_offsets[l][f] = total;
      total += rex::align<VkDeviceSize>(VkDeviceSize(lbw) * lbh * fi.host_block_bytes(), 16);
    }
  }
  VkDeviceSize offset;
  uint8_t* dst_all = Upload(total, 16, offset);
  if (!dst_all) return VK_NULL_HANDLE;
  auto copy_face = [&](uint8_t* dst, uint32_t lbw, uint32_t lbh, const uint8_t* src, size_t src_size,
                       uint32_t row_pitch_bytes, uint32_t ox, uint32_t oy) {
    const size_t row_bytes = size_t(lbw) * fi.bytes_per_block;
    const uint32_t pitch_blocks = row_pitch_bytes >> bpb_log2;
    const bool scratch = REXCVAR_GET(ds2_native_texture_scratch);
    if (scratch && texture_row_scratch_.size() < row_bytes) texture_row_scratch_.resize(row_bytes);
    for (uint32_t y = 0; y < lbh; ++y) {
      uint8_t* const out_row = dst + row_bytes * y;
      uint8_t* row = scratch ? texture_row_scratch_.data() : out_row;
      if (fetch.tiled) {
        for (uint32_t x = 0; x < lbw; ++x) {
          const int32_t o = texture_util::GetTiledOffset2D(int32_t(x + ox), int32_t(y + oy), pitch_blocks, bpb_log2);
          if (o < 0 || size_t(o) + fi.bytes_per_block > src_size) {
            std::memset(row + size_t(x) * fi.bytes_per_block, 0, fi.bytes_per_block);
            continue;
          }
          std::memcpy(row + size_t(x) * fi.bytes_per_block, src + o, fi.bytes_per_block);
        }
      } else {
        const size_t so = size_t(y + oy) * row_pitch_bytes + size_t(ox) * fi.bytes_per_block;
        if (so + row_bytes <= src_size) {
          std::memcpy(row, src + so, row_bytes);
        } else {
          std::memset(row, 0, row_bytes);
        }
      }
      SwapElements(row, row_bytes, fetch.endianness);
      if (row != out_row) std::memcpy(out_row, row, row_bytes);
    }
  };
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t lbw = (std::max(width >> l, 1u) + fi.block_size - 1) / fi.block_size;
    const uint32_t lbh = (std::max(height >> l, 1u) + fi.block_size - 1) / fi.block_size;
    const bool is_base = l == 0;
    const uint32_t stored = is_base ? 0 : std::min(l, layout.packed_level);
    const auto& ll = is_base ? layout.base : layout.mips[stored];
    uint32_t ox = 0, oy = 0, oz = 0;
    if (fetch.packed_mips && l >= layout.packed_level) {
      texture_util::GetPackedMipOffset(width, height, 1, fetch.format, l, ox, oy, oz);
    }
    const uint8_t* level_src = is_base ? guest : guest_mips + layout.mip_offsets_bytes[stored];
    const size_t level_size = is_base ? base_extent
                              : (layout.mip_offsets_bytes[stored] < layout.mips_total_extent_bytes
                                     ? layout.mips_total_extent_bytes - layout.mip_offsets_bytes[stored]
                                     : 0);
    for (uint32_t f = 0; f < 6; ++f) {
      uint8_t* const block_dst = decoded ? texture_block_scratch_.data() : dst_all + face_offsets[l][f];
      const size_t face_off = size_t(f) * ll.array_slice_stride_bytes;
      if (!level_size || face_off >= level_size) {
        std::memset(block_dst, 0, size_t(lbw) * lbh * fi.bytes_per_block);
        if (decoded) {
          DecodeBcnBlocks(fetch.format, fi.bytes_per_block, fi.decoded_texel_bytes, block_dst, lbw, lbh,
                          dst_all + face_offsets[l][f]);
        }
        continue;
      }
      copy_face(block_dst, lbw, lbh, level_src + face_off, level_size - face_off, ll.row_pitch_bytes, ox, oy);
      if (decoded) {
        DecodeBcnBlocks(fetch.format, fi.bytes_per_block, fi.decoded_texel_bytes, block_dst, lbw, lbh,
                        dst_all + face_offsets[l][f]);
      }
    }
  }
  EndRenderPass();
  EnsureGeneral(t.image);
  Barrier();
  if (profiling_) Mark("texture upload cube");
  std::vector<VkBufferImageCopy> regions;
  regions.reserve(levels * 6);
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t lw = std::max(width >> l, 1u), lh = std::max(height >> l, 1u);
    for (uint32_t f = 0; f < 6; ++f) {
      if (face_resolves[l][f]) continue;
      VkBufferImageCopy r = {};
      r.bufferOffset = offset + face_offsets[l][f];
      r.bufferRowLength = (lw + fi.block_size - 1) / fi.block_size * fi.block_size;
      r.bufferImageHeight = (lh + fi.block_size - 1) / fi.block_size * fi.block_size;
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
      r.imageExtent = {lw, lh, 1};
      regions.push_back(r);
    }
  }
  if (!regions.empty()) {
    work_since_barrier_ = true;
    dfn().vkCmdCopyBufferToImage(slot().cmd, slot().upload, t.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                 uint32_t(regions.size()), regions.data());
  }
  uint32_t faces_resolved = 0;
  for (uint32_t l = 0; l < levels; ++l) {
    for (uint32_t f = 0; f < 6; ++f) {
      if (!face_resolves[l][f]) continue;
      VkImageCopy c = {};
      c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
      c.extent = {std::max(width >> l, 1u), std::max(height >> l, 1u), 1};
      work_since_barrier_ = true;
      cmd_copy_image_(slot().cmd, face_resolves[l][f]->image.image, VK_IMAGE_LAYOUT_GENERAL, t.image.image,
                      VK_IMAGE_LAYOUT_GENERAL, 1, &c);
      ++faces_resolved;
    }
  }
  stats_.cube_faces_from_resolves += faces_resolved;
  Barrier();
  ++stats_.texture_uploads;
  stats_.texture_levels += levels * 6;
  stats_.texture_upload_bytes += total;
  if (Tracing()) {
    REXGPU_INFO("[native trace]   cube {:08X} mips {:08X} fmt {} {}x{} levels {} face stride {:X} tiled {} packed {} "
                "faces from resolves {} of {}",
                base, mip_page << 12, uint32_t(fetch.format), width, height, levels, face_stride,
                uint32_t(fetch.tiled), uint32_t(fetch.packed_mips), faces_resolved, levels * 6);
  }
  return GetSwizzleView(t.swizzle_views, t.image, swz);
}

// --- Shaders ----------------------------------------------------------------------------------------------------

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  const uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) return it->second.get();
  auto shader = std::make_unique<VulkanShader>(vulkan_device_, shader_type, hash, host_address, dword_count);
  Shader* raw = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return raw;
}

// --- Draws -----------------------------------------------------------------------------------------------------

void NativeCommandProcessor::LatchRenderScale() {
  if (res_scale_latched_) return;
  res_scale_latched_ = true;
  const double s = REXCVAR_GET(ds2_render_scale);
  if (s >= 0.5 && s < 0.999) {
    res_scale_ = float(s);
    vulkan::g_fragcoord_scale_patch.store(true, std::memory_order_relaxed);
  }
  REXGPU_INFO("[native] internal resolution: {:.2f} ({}x{} for the 1280x720 scene)", double(res_scale_),
              Sc(uint32_t(1280)), Sc(uint32_t(720)));
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info, bool major_mode_explicit) {
  (void)major_mode_explicit;
  LatchRenderScale();
  if (!msaa_force_clear_) msaa_replay_clear_ = false;
  const RegisterFile& regs = *register_file_;
  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) return IssueCopy();
  if (halfres_pending_rt_) {
    Shader* aps = active_pixel_shader();
    if (!aps || !IsHalfResPixelShader(aps->ucode_data_hash())) FlushHalfRes();
  }
  ScopedUs draw_timer(stats_.us_draws);
  const uint64_t draw_start_us = NowUs();
  const uint64_t draw_start_textures = stats_.us_textures, draw_start_pipelines = stats_.us_pipelines;
  ++stats_.prim_counts[uint32_t(prim_type) & 31];
  if (!frame_open_ && !BeginFrame()) return false;

  auto* vs = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vs) {
    ++stats_.draws_skipped;
    ++stats_.skip_no_vs;
    return true;
  }
  if (!vs->is_ucode_analyzed()) vs->AnalyzeUcode(ucode_disasm_buffer_);
  const bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  const bool rasterization = draw_util::IsRasterizationPotentiallyDone(regs, polygonal);
  if (!rasterization || regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    ++stats_.draws_skipped;
    ++(rasterization ? stats_.skip_no_pitch : stats_.skip_no_raster);
    return true;
  }
  VulkanShader* ps = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    ps = static_cast<VulkanShader*>(active_pixel_shader());
    if (ps) {
      if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(ucode_disasm_buffer_);
      if (!draw_util::IsPixelShaderNeededWithRasterization(*ps, regs)) ps = nullptr;
    }
  }

  VkPrimitiveTopology topology;
  switch (prim_type) {
    case xenos::PrimitiveType::kLineList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case xenos::PrimitiveType::kTriangleFan:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case xenos::PrimitiveType::kPointList:
      // The translator's point sprite mode: 4 host vertices per point in a strip.
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case xenos::PrimitiveType::kRectangleList:
      // The translator's rectangle mode: 4 host vertices per rectangle in a strip, the vertex shader runs the guest
      // shader for the 3 guest vertices and makes the 4th corner.
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case xenos::PrimitiveType::kQuadList:
      // Expanded on the CPU into a triangle list of indices below (guest vertices are still fetched by index).
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    default:
      ++stats_.draws_unsupported_prim;
      return true;
  }
  if (prim_type == xenos::PrimitiveType::kPointList && index_count > 65535 * 4) {
    ++stats_.draws_unsupported_prim;
    return true;
  }

  // Shader translations.
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t ps_param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      ps ? (vs->writes_interpolators() &
            ps->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(), regs.Get<reg::SQ_CONTEXT_MISC>(),
                                         ps_param_gen_pos))
         : 0;
  SpirvShaderTranslator::Modification vs_mod(translator_->GetDefaultVertexShaderModification(
      vs->GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      prim_type == xenos::PrimitiveType::kRectangleList ? Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
      : prim_type == xenos::PrimitiveType::kPointList   ? Shader::HostVertexShaderType::kPointListAsTriangleStrip
                                                        : Shader::HostVertexShaderType::kVertex));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  vs_mod.vertex.user_clip_plane_count = 0;
  vs_mod.vertex.user_clip_plane_cull = 0;
  vs_mod.vertex.output_point_parameters =
      prim_type == xenos::PrimitiveType::kPointList && ps_param_gen_pos != UINT32_MAX ? 1 : 0;
  vs_mod.vertex.vertex_kill_and = 0;
  SpirvShaderTranslator::Modification ps_mod(0);
  if (ps) {
    ps_mod = SpirvShaderTranslator::Modification(translator_->GetDefaultPixelShaderModification(
        ps->GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid = 0;
    const auto alpha_control = regs.Get<reg::RB_COLORCONTROL>();
    ps_mod.pixel.alpha_test_always = REXCVAR_GET(ds2_native_alpha_specialization) &&
        (!alpha_control.alpha_test_enable || alpha_control.alpha_func == xenos::CompareFunction::kAlways);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = prim_type == xenos::PrimitiveType::kPointList ? 1 : 0;
    }
    // v55: early depth/stencil tests (as the emulation's pipeline cache does): without this every pixel shader
    // carries the dynamic alpha test (OpKill) and alpha to coverage code, which forces late Z, so hidden
    // fragments are shaded too.
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(ucode_disasm_buffer_);
    ps_mod.pixel.depth_stencil_mode =
        REXCVAR_GET(ds2_native_early_z) && ps->implicit_early_z_write_allowed() &&
                (!ps->writes_color_target(0) ||
                 !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
    if (ps_mod.pixel.depth_stencil_mode == DepthStencilMode::kEarlyHint) ++stats_.early_z_draws;
  }
  auto* vst = static_cast<VulkanShader::VulkanTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
  auto* pst = ps ? static_cast<VulkanShader::VulkanTranslation*>(ps->GetOrCreateTranslation(ps_mod.value)) : nullptr;
  // RC63: the layout is analysed only when the experimental path is on (RC62 console logs: 0 % eligible draws).
  static const NativeVertexLayout kNoNativeVertexLayout;
  const bool native_vertex_cvar = REXCVAR_GET(ds2_native_vertex_inputs);
  const NativeVertexLayout* native_plan_ptr = &kNoNativeVertexLayout;
  if (native_vertex_cvar) {
    auto plan_it = native_vertex_layouts_.find(vs->ucode_data_hash());
    if (plan_it == native_vertex_layouts_.end()) {
      plan_it = native_vertex_layouts_.emplace(vs->ucode_data_hash(), BuildNativeVertexLayout(*vs)).first;
    }
    native_plan_ptr = &plan_it->second;
  }
  const NativeVertexLayout& native_plan = *native_plan_ptr;
  bool native_vertex = native_vertex_cvar && native_plan.valid &&
      prim_type == xenos::PrimitiveType::kTriangleList &&
      !regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
      regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET) == 0 &&
      regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX) == 0 && index_count && index_count <= (1u << 20) &&
      native_plan.bindings.size() <= 8 &&
      native_plan.words.size() <= 16;
  uint32_t native_max_index = index_count ? index_count - 1 : 0;
  native_indices_.clear();
  if (native_vertex && index_buffer_info && index_buffer_info->guest_base) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    const auto endian = index_buffer_info->endianness;
    const uint32_t bytes_per_index = is32 ? 4 : 2;
    const uint64_t bytes = uint64_t(index_count) * bytes_per_index;
    const uint8_t* data = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
    native_vertex = data && (!index_buffer_info->length || bytes <= index_buffer_info->length) &&
        (endian == xenos::Endian::kNone || (is32 ? endian == xenos::Endian::k8in32 : endian == xenos::Endian::k8in16));
    if (native_vertex) {
      native_indices_.resize(index_count);
      native_max_index = 0;
      for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t value = 0;
        if (is32) { std::memcpy(&value, data + size_t(i) * 4, 4); }
        else { uint16_t word; std::memcpy(&word, data + size_t(i) * 2, 2); value = word; }
        value = xenos::GpuSwap(value, endian);
        native_indices_[i] = value;
        native_max_index = std::max(native_max_index, value);
      }
    }
  }
  native_vertex = native_vertex && native_max_index <= 0x7FFFFF &&
      native_max_index <= regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  if (native_vertex) {
    for (const auto& binding : native_plan.bindings) {
      bool uploaded_binding = false;
      for (const auto& original : vs->vertex_bindings()) {
        if (original.fetch_constant == binding.fetch_constant && original.stride_words * 4 == binding.stride) uploaded_binding = true;
      }
      if (!uploaded_binding) native_vertex = false;
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      const uint64_t required = uint64_t(native_max_index + 1) * binding.stride;
      if (required > (uint64_t(fetch.size) << 2) ||
          binding.stride > 2048 ||
          !memory_->TranslatePhysical<const uint8_t*>(uint32_t(fetch.address) << 2)) native_vertex = false;
    }
    for (const auto& word : native_plan.words) {
      if (word.offset > 2047) native_vertex = false;
    }
  }
  if (native_vertex) {
    vs_mod.vertex.native_vertex_inputs = 1;
    vst = static_cast<VulkanShader::VulkanTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
  }
  uint64_t translate_start = NowUs();
  if (!restore_done_.empty() || !restore_pending_.empty()) DrainRestores(false, nullptr);
  {
    const bool shader_async = !native_vertex && prim_type != xenos::PrimitiveType::kRectangleList && index_count > 6;
    const bool vs_ready = PrepareTranslation(vst, shader_async);
    const bool ps_ready = PrepareTranslation(pst, shader_async);
    if (!vs_ready || !ps_ready) {
      stats_.us_pipelines += NowUs() - translate_start;
      stats_.us_translate += NowUs() - translate_start;
      ++stats_.draws_skipped;
      ++stats_.draws_shader_pending;
      return true;
    }
  }
  if (native_vertex && (!vst->is_valid() || !vst->shader_module())) {
    native_vertex = false;
    vs_mod.vertex.native_vertex_inputs = 0;
    vst = static_cast<VulkanShader::VulkanTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
    if (!vst->is_translated()) translator_->TranslateAnalyzedShader(*vst);
    if (vst->is_valid()) vst->GetOrCreateShaderModule();
  }
  stats_.us_pipelines += NowUs() - translate_start;
  stats_.us_translate += NowUs() - translate_start;
  if (!vst->is_valid() || !vst->shader_module() || (pst && (!pst->is_valid() || !pst->shader_module()))) {
    ++stats_.draws_failed;
    return true;
  }

  // Render targets.
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t color_mask = draw_util::GetNormalizedColorMask(regs, ps ? ps->writes_color_targets() : 0);
  // MSAA render targets live in sample space: a 4x 640x360 target is a 1280x720 image, so the 1x and 4x views of the
  // same EDRAM range (DS2 renders shadow maps and half-resolution passes that way) are the same image. Draws into
  // them are rasterized at the sample resolution (like a 2x2 resolution scale).
  uint32_t sx = surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1;
  uint32_t sy = surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 2 : 1;
  const uint32_t sample_pitch = surface_info.surface_pitch * sx;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  draw_util::Scissor scissor_pixels = scissor;
  // RC71: host 4x MSAA candidates (the lookups below then leave the 4x copies as they are).
  const bool msaa_candidate = msaa_supported_ && REXCVAR_GET(ds2_native_msaa_host) &&
                              surface_info.msaa_samples == xenos::MsaaSamples::k4X;
  msaa_lookup_ = msaa_candidate;
  scissor.offset[0] *= sx;
  scissor.extent[0] *= sx;
  scissor.offset[1] *= sy;
  scissor.extent[1] *= sy;
  const uint32_t height_hint = std::max<uint32_t>(scissor.offset[1] + scissor.extent[1], 8);
  // v109: the lowest pixel row of a rectangle-list draw in window coordinates (clears), NaN otherwise; bounds the
  // rows a draw writes (EDRAM ownership, NativeImage::epoch_rows; v110: also the rows of a 4x copy to export).
  float rect_max_y = std::numeric_limits<float>::quiet_NaN();
  // v111: one rectangle only (3 vertices, not indexed); a list of several (UI, sprites) is bounded by the scissor.
  if (prim_type == xenos::PrimitiveType::kRectangleList && index_count == 3 && !index_buffer_info &&
      !(regs[XE_GPU_REG_PA_CL_VTE_CNTL] & 0xC) && !vs->vertex_bindings().empty()) {
    const auto& binding = vs->vertex_bindings().front();
    xenos::xe_gpu_vertex_fetch_t vf;
    std::memcpy(&vf, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + binding.fetch_constant * 2], sizeof(vf));
    const uint32_t stride_words = std::max<uint32_t>(binding.stride_words, 3);
    const uint32_t size = uint32_t(vf.size) << 2;
    const uint32_t* vtx = memory_->TranslatePhysical<const uint32_t*>(uint32_t(vf.address) << 2);
    if (vtx && size >= (2 * stride_words + 2) * 4) {
      for (uint32_t v = 0; v < 3; ++v) {
        const float y = xenos::GpuSwap(rex::memory::Reinterpret<float>(vtx[v * stride_words + 1]), vf.endian);
        if (std::isnan(rect_max_y) || y > rect_max_y) rect_max_y = y;
      }
      // v113: DS2 draws with the window offset enabled (it is 0); add it like the viewport does.
      if (!std::isnan(rect_max_y) && regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
        rect_max_y += float(regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset);
      }
    }
  }
  // v107: a draw into the scene's render targets at the reduced internal resolution.
  const bool draw_scaled = IsScaledPitch(sample_pitch);
  if (draw_scaled) {
    ScaleScissor(scissor);
    ScaleScissor(scissor_pixels);
  }
  NativeImage* color_rts[4] = {};
  VkFormat color_formats[4] = {};
  uint32_t color_count = 0;
  uint32_t write_mask = 0;
  uint32_t blend[4] = {};
  uint32_t rt_width = UINT32_MAX, rt_height = UINT32_MAX;
  uint32_t last_used_rt = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if ((color_mask >> (4 * i)) & 0xF) last_used_rt = i + 1;
  }
  for (uint32_t i = 0; i < last_used_rt; ++i) {
    const auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    NativeImage* rt = GetRenderTarget(ci.color_base, sample_pitch, uint32_t(ci.color_format), false, height_hint);
    if (!rt) {
      msaa_lookup_ = false;
      ++stats_.draws_failed;
      return true;
    }
    color_rts[color_count] = rt;
    color_formats[color_count] = rt->format;
    write_mask |= ((color_mask >> (4 * i)) & 0xF) << (4 * color_count);
    const uint32_t bc = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
    blend[color_count] = bc;
    rt_width = std::min(rt_width, rt->width);
    rt_height = std::min(rt_height, rt->height);
    ++color_count;
  }
  NativeImage* depth_rt = nullptr;
  if (depth_control.z_enable || depth_control.stencil_enable) {
    const auto di = regs.Get<reg::RB_DEPTH_INFO>();
    depth_rt = GetRenderTarget(di.depth_base, sample_pitch, uint32_t(di.depth_format), true, height_hint);
    if (depth_rt) {
      rt_width = std::min(rt_width, depth_rt->width);
      rt_height = std::min(rt_height, depth_rt->height);
    }
  }
  msaa_lookup_ = false;
  // RC71: a 4x pass with color targets (or a 4x pass on a depth buffer whose 4x copy is current) is drawn into the
  // 4x MSAA copies. Anything else uses the sample-space images, so their 4x copies are exported first.
  bool host_msaa = false;
  // A full clear draw (rectangle, depth always written, stencil fully replaced, all color channels written
  // without blending) makes importing the old contents unnecessary.
  bool full_clear = msaa_candidate && REXCVAR_GET(ds2_native_msaa_skip_clear_import) &&
                    prim_type == xenos::PrimitiveType::kRectangleList;
  if (full_clear && depth_rt) {
    const auto srm_clear = regs.Get<reg::RB_STENCILREFMASK>();
    full_clear = depth_control.z_enable && depth_control.z_write_enable &&
                 depth_control.zfunc == xenos::CompareFunction::kAlways && depth_control.stencil_enable &&
                 depth_control.stencilfunc == xenos::CompareFunction::kAlways &&
                 depth_control.stencilzpass == xenos::StencilOp::kReplace && srm_clear.stencilwritemask == 0xFF;
  }
  for (uint32_t i = 0; i < color_count && full_clear; ++i) {
    full_clear = ((write_mask >> (4 * i)) & 0xF) == 0xF && blend[i] == 0x00010001u;
  }
  if (msaa_candidate) {
    if (color_count) {
      // RC73: geometry always goes to the 4x copies. A clear rectangle does too while the 4x copy is current;
      // otherwise it stays in sample space (DS2 also clears in 4x and then draws at 1x: a 4x copy for a lone
      // clear would cost an export and save nothing).
      host_msaa = true;
      if (REXCVAR_GET(ds2_native_msaa_clear_replay) && prim_type == xenos::PrimitiveType::kRectangleList &&
          !msaa_force_clear_) {
        auto sit = msaa_shadows_.find(color_rts[0]->image);
        host_msaa = sit != msaa_shadows_.end() && sit->second.valid && sit->second.newer;
        // A full clear outside an active 4x copy: drawn in sample space, then replayed into the 4x copies (they
        // then match the sample-space image without an import, and need no export until geometry is drawn).
        if (!host_msaa && full_clear) msaa_replay_clear_ = true;
      }
    } else if (depth_rt) {
      auto sit = msaa_shadows_.find(depth_rt->image);
      host_msaa = sit != msaa_shadows_.end() && sit->second.valid;
    }
    if (host_msaa) {
      for (uint32_t i = 0; i < color_count; ++i) {
        if (color_rts[i]->width < 2 || color_rts[i]->height < 2) host_msaa = false;
      }
    }
    if (!host_msaa) {
      for (uint32_t i = 0; i < color_count; ++i) ExportMsaaShadow(*color_rts[i], true);
      if (depth_rt) ExportMsaaShadow(*depth_rt, true);
    }
  }
  if (host_msaa) {
    NativeImage* shadows[5] = {};
    // Rows of the 4x copy an import has to fill: the drawn area (8-row aligned, all of it at least once).
    const uint32_t import_rows = rex::align<uint32_t>(scissor_pixels.offset[1] + scissor_pixels.extent[1], 8);
    for (uint32_t i = 0; i < color_count && host_msaa; ++i) {
      shadows[i] = GetMsaaShadow(*color_rts[i], !full_clear, import_rows);
      if (!shadows[i]) host_msaa = false;
    }
    if (host_msaa && depth_rt) {
      shadows[4] = GetMsaaShadow(*depth_rt, !full_clear, import_rows);
      if (!shadows[4]) host_msaa = false;
    }
    if (Tracing()) {
      REXGPU_INFO("[native trace]   host 4x MSAA: {} (full clear {}, import rows {})", host_msaa ? "on" : "failed",
                  full_clear, import_rows);
    }
    if (host_msaa) {
      rt_width = rt_height = UINT32_MAX;
      uint32_t drawn_rows = scissor_pixels.offset[1] + scissor_pixels.extent[1];
      if (!std::isnan(rect_max_y) && REXCVAR_GET(ds2_native_convert_written_rows)) {
        // v110: a clear rectangle: only its rows (pixel space of the 4x copy).
        float y = rect_max_y + 0.5f;
        if (draw_scaled) y *= res_scale_;
        drawn_rows = std::min(drawn_rows, uint32_t(std::max(std::ceil(y), 1.0f)));
      }
      for (uint32_t i = 0; i < color_count; ++i) {
        MsaaShadow& csh = msaa_shadows_[color_rts[i]->image];
        csh.newer = !msaa_force_clear_;
        if (!msaa_force_clear_) csh.dirty_rows = std::max(csh.dirty_rows, drawn_rows);
        color_rts[i] = shadows[i];
        rt_width = std::min(rt_width, shadows[i]->width);
        rt_height = std::min(rt_height, shadows[i]->height);
      }
      if (depth_rt) {
        MsaaShadow& dsh = msaa_shadows_[depth_rt->image];
        dsh.newer = !msaa_force_clear_;
        if (!msaa_force_clear_) dsh.dirty_rows = std::max(dsh.dirty_rows, drawn_rows);
        depth_rt = shadows[4];
        rt_width = std::min(rt_width, depth_rt->width);
        rt_height = std::min(rt_height, depth_rt->height);
      }
      // Pixel space from here on (viewport, scissor, point sizes).
      sx = sy = 1;
      scissor = scissor_pixels;
      ++stats_.msaa_draws;
    } else {
      for (uint32_t i = 0; i < color_count; ++i) ExportMsaaShadow(*color_rts[i], true);
      if (depth_rt) ExportMsaaShadow(*depth_rt, true);
    }
  }
  if (!color_count && !depth_rt) {
    ++stats_.draws_skipped;
    ++stats_.skip_no_target;
    if (occlusion_active_ >= 0) occlusion_unmeasured_ = true;  // every sample would pass: not measurable here
    return true;
  }
  // RC82/84: reduced-resolution particles (see halfres_targets_). Accepted blends: color = src * (ONE or
  // SRC_ALPHA) + dst * (ONE or ONE_MINUS_SRC_ALPHA), alpha = dst * the same factor as color (DS2 fog: additive
  // 01000106 at bonfires, "over" 07000706 for fog walls). The copy starts as (0, 0, 0, 1): its color is what the
  // particles add, its alpha what remains of the scene, so compositing is scene * a + rgb.
  NativeImage* halfres_full_rt = nullptr;
  halfres_draw_ = false;
  {
    const uint32_t b = blend[0];
    const uint32_t csrc = b & 0x1Fu, ccomb = (b >> 5) & 7u, cdst = (b >> 8) & 0x1Fu;
    const uint32_t asrc = (b >> 16) & 0x1Fu, acomb = (b >> 21) & 7u, adst = (b >> 24) & 0x1Fu;
    // v105: 07060706 (alpha source factor = color source factor) too; its alpha source term is dropped in the copy.
    const bool blend_ok = (csrc == 1 || csrc == 6) && ccomb == 0 && (cdst == 1 || cdst == 7) &&
                          (asrc == 0 || (asrc == csrc && REXCVAR_GET(ds2_native_halfres_alpha_over))) &&
                          acomb == 0 && adst == cdst;
    const uint32_t d = std::clamp<uint32_t>(uint32_t(std::max(REXCVAR_GET(ds2_native_halfres_divisor), 1)), 1, 8);
    uint32_t mode = ps ? HalfResMode(ps->ucode_data_hash()) : 0;
    // A depth buffer at reduced resolution is only used for draws that test but do not write depth or stencil.
    const bool wants_depth = mode == 2 && depth_rt && depth_control.z_enable;
    if (mode == 2 && depth_rt && (depth_control.z_write_enable || depth_control.stencil_enable)) mode = 0;
    if (wants_depth && (depth_rt->width < 2 * d || depth_rt->height < 2 * d)) mode = 0;
    // v105: in a host 4x MSAA pass too (without a reduced depth buffer: that would read the 4x depth copy).
    const bool msaa_ok = !host_msaa || (!wants_depth && REXCVAR_GET(ds2_native_halfres_msaa) &&
                                        color_rts[0]->samples == 4);
    if (mode && ps && color_count == 1 && msaa_ok && sx == 1 && sy == 1 && !msaa_force_clear_ && d > 1 && blend_ok &&
        (color_rts[0]->format == VK_FORMAT_R16G16B16A16_SFLOAT ||
         color_rts[0]->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) &&
        color_rts[0]->width >= 2 * d &&
        color_rts[0]->height >= 2 * d) {
      HalfResTarget& h = halfres_targets_[color_rts[0]->image];
      // The reduced copy is always RGBA16F: its alpha holds what remains of the scene (v106: also for B10G11R11
      // targets, ds2_native_7e3_as_r11g11b10).
      if (h.image.image && h.image.width != color_rts[0]->width / d) {
        if (halfres_pending_half_ == &h.image) FlushHalfRes();
        DestroyImage(h.image, true);
        h.image = NativeImage();
      }
      if (!h.image.image) {
        if (!CreateImage(h.image, VK_FORMAT_R16G16B16A16_SFLOAT, color_rts[0]->width / d, color_rts[0]->height / d, false,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
          DestroyImage(h.image, false);
          h.image = NativeImage();
        } else {
          EnsureGeneral(h.image);
        }
      }
      if (h.image.image) {
        NativeImage* reduced_depth = nullptr;
        if (wants_depth) {
          HalfResTarget& hd = halfres_depth_targets_[depth_rt->image];
          if (hd.image.image && (hd.image.width != depth_rt->width / d || hd.image.format != depth_rt->format)) {
            if (halfres_pending_depth_src_) halfres_pending_depth_src_ = nullptr;
            DestroyImage(hd.image, true);
            hd.image = NativeImage();
          }
          if (!hd.image.image) {
            if (!CreateImage(hd.image, depth_rt->format, depth_rt->width / d, depth_rt->height / d, true,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
              DestroyImage(hd.image, false);
              hd.image = NativeImage();
            } else {
              EnsureGeneral(hd.image);
            }
          }
          if (hd.image.image) reduced_depth = &hd.image;
        }
        if (!wants_depth || reduced_depth) {
          if (halfres_pending_rt_ != color_rts[0]) {
            FlushHalfRes();
            VkClearValue start = {};
            start.color.float32[3] = 1.0f;
            ClearRect(h.image, start, {{0, 0}, {h.image.width, h.image.height}}, false);
            halfres_pending_rt_ = color_rts[0];
            halfres_pending_half_ = &h.image;
            halfres_pending_rect_ = {};
            halfres_pending_depth_src_ = nullptr;
          }
          if (reduced_depth && halfres_pending_depth_src_ != depth_rt) {
            // Nearest depth of each block for "greater" tests (reversed depth), farthest for "less": the particles
            // stay hidden behind the edges of closer geometry instead of covering it.
            const uint32_t f = uint32_t(depth_control.zfunc);
            const bool use_max = f == uint32_t(xenos::CompareFunction::kGreater) ||
                                 f == uint32_t(xenos::CompareFunction::kGreaterEqual);
            DownsampleDepth(*depth_rt, *reduced_depth, d, use_max);
            halfres_pending_depth_src_ = depth_rt;
          }
          halfres_full_rt = color_rts[0];
          color_rts[0] = &h.image;
          color_formats[0] = h.image.format;
          depth_rt = reduced_depth;
          rt_width = h.image.width;
          rt_height = h.image.height;
          if (reduced_depth) {
            rt_width = std::min(rt_width, reduced_depth->width);
            rt_height = std::min(rt_height, reduced_depth->height);
          }
          halfres_draw_ = true;
          halfres_divisor_ = d;
          halfres_mode_ = mode;
          host_msaa = false;  // v105: the reduced copy is single-sampled; the composite writes the 4x copy
        }
      }
    }
  }
  if (!halfres_draw_) FlushHalfRes();
  VkRenderPass pass = GetRenderPass(color_formats, color_count, depth_rt ? depth_rt->format : VK_FORMAT_UNDEFINED,
                                    host_msaa ? 4 : 1);
  if (!pass) {
    ++stats_.draws_failed;
    return true;
  }

  // Textures (may record uploads, so before the render pass begins).
  const auto& vs_textures = vs->GetTextureBindingsAfterTranslation();
  const auto& vs_samplers = vs->GetSamplerBindingsAfterTranslation();
  static const std::vector<VulkanShader::TextureBinding> kNoTextures;
  static const std::vector<VulkanShader::SamplerBinding> kNoSamplers;
  const auto& ps_textures = ps ? ps->GetTextureBindingsAfterTranslation() : kNoTextures;
  const auto& ps_samplers = ps ? ps->GetSamplerBindingsAfterTranslation() : kNoSamplers;
  auto& image_infos = draw_image_infos_;  // RC87: reused, no allocation per draw
  image_infos.clear();
  image_infos.reserve(vs_textures.size() + vs_samplers.size() + ps_textures.size() + ps_samplers.size());
  auto gather = [&](const std::vector<VulkanShader::TextureBinding>& tb,
                    const std::vector<VulkanShader::SamplerBinding>& sb) {
    for (const auto& b : tb) {
      bool resolved;
      VkImageView view = GetTextureView(b.fetch_constant, b.dimension, resolved);
      image_infos.push_back({VK_NULL_HANDLE, view ? view : white_view_, VK_IMAGE_LAYOUT_GENERAL});
    }
    for (const auto& s : sb) {
      image_infos.push_back(
          {GetSampler(s, regs.GetTextureFetch(s.fetch_constant)), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    }
  };
  gather(vs_textures, vs_samplers);
  gather(ps_textures, ps_samplers);

  const PipelineLayoutEntry* layout = GetPipelineLayout(uint32_t(vs_textures.size()), uint32_t(vs_samplers.size()),
                                                        uint32_t(ps_textures.size()), uint32_t(ps_samplers.size()));
  if (!layout) {
    ++stats_.draws_failed;
    return true;
  }

  // Pipeline.
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs_hash = vs->ucode_data_hash();
  key.vs_modification = vs_mod.value;
  key.ps_hash = ps ? ps->ucode_data_hash() : 0;
  key.ps_modification = ps_mod.value;
  key.render_pass = reinterpret_cast<uint64_t>(pass);
  key.topology = uint32_t(topology);
  const bool indexed = index_buffer_info && index_buffer_info->guest_base;
  key.primitive_restart =
      prim_type == xenos::PrimitiveType::kRectangleList || prim_type == xenos::PrimitiveType::kPointList ||
              (indexed && regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
       (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
        topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN))
          ? 1
          : 0;
  const auto sc_mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (polygonal) {
    key.cull_mode = (sc_mode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (sc_mode.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_face_cw = sc_mode.face ? 1 : 0;
  }
  if (depth_rt) {
    key.depth_test = depth_control.z_enable;
    key.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    key.depth_func = depth_control.z_enable ? uint32_t(depth_control.zfunc) : uint32_t(VK_COMPARE_OP_ALWAYS);
    key.stencil_enable = depth_control.stencil_enable;
    if (depth_control.stencil_enable) {
      key.stencil_front = uint32_t(depth_control.stencilfail) | (uint32_t(depth_control.stencilzpass) << 3) |
                          (uint32_t(depth_control.stencilzfail) << 6) | (uint32_t(depth_control.stencilfunc) << 9);
      key.stencil_back = (polygonal && depth_control.backface_enable)
                             ? uint32_t(depth_control.stencilfail_bf) |
                                   (uint32_t(depth_control.stencilzpass_bf) << 3) |
                                   (uint32_t(depth_control.stencilzfail_bf) << 6) |
                                   (uint32_t(depth_control.stencilfunc_bf) << 9)
                             : key.stencil_front;
    }
  }
  for (uint32_t i = 0; i < color_count; ++i) key.blend[i] = blend[i];
  // v105: the reduced copy's alpha is what remains of the scene: dst * factor only (07060706 adds src_a * src_a).
  if (halfres_draw_) key.blend[0] &= ~(0x1Fu << 16);
  key.write_mask = write_mask;
  key.color_count = color_count;
  key.has_ps = ps ? 1 : 0;
  key.depth_clamp = regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable ? 1 : 0;
  key.samples = host_msaa ? 4 : 0;
  key.samples_reserved = (draw_scaled && ps) ? 1u : 0u;  // v107: pixel shader sees guest pixel positions
  if (native_vertex) {
    key.native_binding_count = uint32_t(native_plan.bindings.size());
    key.native_attribute_count = uint32_t(native_plan.words.size());
    for (uint32_t i = 0; i < key.native_binding_count; ++i) {
      key.native_bindings[i] = {i, native_plan.bindings[i].stride, VK_VERTEX_INPUT_RATE_VERTEX};
    }
    for (uint32_t i = 0; i < key.native_attribute_count; ++i) {
      key.native_attributes[i] = {i, native_plan.words[i].binding, VK_FORMAT_R32_UINT, native_plan.words[i].offset};
    }
  }
  key.texture_layout = uint32_t(vs_textures.size()) | (uint32_t(vs_samplers.size()) << 8) |
                       (uint32_t(ps_textures.size()) << 16) | (uint32_t(ps_samplers.size()) << 24);
  // Full-screen passes (rectangles, a few vertices) wait for their pipeline: skipping one would flash the frame.
  const bool allow_async = prim_type != xenos::PrimitiveType::kRectangleList && index_count > 6;
  bool pipeline_pending = false;
  if (REXCVAR_GET(ds2_native_fold_texture_signs)) {
    // RC96: the same words as the system constants' texture_swizzled_signs.
    for (const auto* tb : {&vs_textures, &ps_textures}) {
      for (const auto& b : *tb) {
        const uint32_t i = b.fetch_constant;
        key.texture_signs[(i >> 2) & 7] |=
            uint32_t(texture_util::SwizzleSigns(regs.GetTextureFetch(i))) << (8 * (i & 3));
      }
    }
  }
  VkPipeline pipeline = GetPipeline(key, vst, pst, layout->layout, allow_async && !native_vertex, pipeline_pending);
  if (!pipeline && native_vertex) {
    native_vertex = false;
    vs_mod.vertex.native_vertex_inputs = 0;
    vst = static_cast<VulkanShader::VulkanTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
    if (!vst->is_translated()) translator_->TranslateAnalyzedShader(*vst);
    if (vst->is_valid()) vst->GetOrCreateShaderModule();
    key.vs_modification = vs_mod.value;
    key.native_binding_count = key.native_attribute_count = 0;
    std::memset(key.native_bindings, 0, sizeof(key.native_bindings));
    std::memset(key.native_attributes, 0, sizeof(key.native_attributes));
    if (vst->is_valid() && vst->shader_module()) pipeline = GetPipeline(key, vst, pst, layout->layout, false, pipeline_pending);
  }
  if (pipeline_pending) {
    ++stats_.draws_pipeline_pending;
    return true;
  }
  if (!pipeline) {
    ++stats_.draws_failed;
    return true;
  }

  const uint64_t draw_frame = frame_;
  // Vertex data: copy every vertex buffer the shader fetches into the upload buffer, patch its fetch constant.
  const uint64_t geometry_start = NowUs();
  stats_.us_prelude += (geometry_start - draw_start_us) - (stats_.us_textures - draw_start_textures) -
                       (stats_.us_pipelines - draw_start_pipelines);
  VkDeviceSize native_vertex_offsets[8] = {};
  uint32_t fetch_constants[6 * 32];
  std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(fetch_constants));
  float rect_first_z = std::numeric_limits<float>::quiet_NaN();
  for (const auto& binding : vs->vertex_bindings()) {
    const uint32_t fc = binding.fetch_constant;
    xenos::xe_gpu_vertex_fetch_t vf;
    std::memcpy(&vf, &fetch_constants[fc * 2], sizeof(vf));
    const uint32_t address = uint32_t(vf.address) << 2;
    uint32_t size = uint32_t(vf.size) << 2;
    if (prim_type == xenos::PrimitiveType::kRectangleList && std::isnan(rect_first_z) && size >= 12) {
      if (const uint32_t* vtx = memory_->TranslatePhysical<const uint32_t*>(address)) {
        rect_first_z = xenos::GpuSwap(rex::memory::Reinterpret<float>(vtx[2]), vf.endian);
      }
    }
    if (Tracing()) {
      REXGPU_INFO("[native trace]   draw {} vfetch {} addr {:08X} size {} stride {}", draws_this_frame_, fc, address,
                  size, binding.stride_words * 4);
      if (prim_type == xenos::PrimitiveType::kRectangleList) {
        const uint32_t* vtx = memory_->TranslatePhysical<const uint32_t*>(address);
        // RC73: the first 3 words of each of the 3 vertices (x y z), at the binding stride.
        const uint32_t stride_words = std::max<uint32_t>(binding.stride_words, 3);
        float f[9] = {};
        for (uint32_t v = 0; vtx && v < 3; ++v) {
          for (uint32_t c = 0; c < 3 && (v * stride_words + c) * 4 < size; ++c) {
            f[v * 3 + c] = xenos::GpuSwap(rex::memory::Reinterpret<float>(vtx[v * stride_words + c]), vf.endian);
          }
        }
        REXGPU_INFO("[native trace]     rect vertices ({} {} {}) ({} {} {}) ({} {} {}) surface pitch {} msaa {}",
                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8],
                    uint32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch),
                    uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples));
        // v112: the rest of the first vertex (DS2's clear rectangles carry their color there) and the exponent
        // bias of the first color target (the clear color is scaled by it).
        float extra[4] = {};
        for (uint32_t c = 0; vtx && c < 4 && (3 + c) < stride_words && (3 + c) * 4 < size; ++c) {
          extra[c] = xenos::GpuSwap(rex::memory::Reinterpret<float>(vtx[3 + c]), vf.endian);
        }
        const auto ci0 = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
        REXGPU_INFO("[native trace]     rect vertex 0 data ({} {} {} {}) stride {} | RT0 format {} exp bias {}",
                    extra[0], extra[1], extra[2], extra[3], stride_words, uint32_t(ci0.color_format),
                    int32_t(ci0.color_exp_bias));
      }
    }
    if (!size) {
      // No size in the fetch constant: cover the vertices the draw can index.
      const uint32_t max_index = std::min<uint32_t>(regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX), 0xFFFFF);
      size = std::min<uint32_t>((max_index + 1) * binding.stride_words * 4, 16u << 20);
      if (!size) continue;
    }
    const uint64_t vkey = (uint64_t(address) << 32) | size;
    VkDeviceSize offset;
    auto vit = frame_vertex_uploads_.find(vkey);
    if (vit != frame_vertex_uploads_.end()) {
      offset = vit->second;
    } else {
      const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(address);
      uint8_t* dst = src ? Upload(size, 256, offset) : nullptr;
      if (!dst) {
        ++stats_.draws_failed;
        return true;
      }
      std::memcpy(dst, src, size);
      frame_vertex_uploads_.emplace(vkey, offset);
    }
    if (native_vertex) {
      for (size_t i = 0; i < native_plan.bindings.size(); ++i) {
        if (native_plan.bindings[i].fetch_constant == fc) native_vertex_offsets[i] = offset;
      }
    }
    vf.address = uint32_t(offset >> 2);
    std::memcpy(&fetch_constants[fc * 2], &vf, sizeof(vf));
  }

  // Indices: guest indices (still in the guest byte order - the vertex shader swaps them, vertex_index_endian), or
  // generated for rectangle / quad lists.
  VkDeviceSize index_offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  uint32_t draw_count = index_count;
  bool use_indices = indexed;
  xenos::Endian index_endian = xenos::Endian::kNone;
  if (indexed) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    index_type = is32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    index_endian = index_buffer_info->endianness;
    if (is32 && index_endian == xenos::Endian::k8in16) index_endian = xenos::Endian::kNone;  // as in the emulation
    if (!is32 && index_endian != xenos::Endian::kNone && index_endian != xenos::Endian::k8in16) {
      index_endian = index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
    }
  }
  uint32_t index_load_flags = 0;
  uint32_t index_load_address = 0;
  if (prim_type == xenos::PrimitiveType::kRectangleList || prim_type == xenos::PrimitiveType::kPointList) {
    const uint32_t rects = index_count / (prim_type == xenos::PrimitiveType::kPointList ? 1 : 3);
    if (!rects) {
      ++stats_.draws_skipped;
      return true;
    }
    if (indexed) {
      // The vertex shader loads the guest indices itself from the "shared memory" (upload buffer).
      const bool is32 = index_type == VK_INDEX_TYPE_UINT32;
      const VkDeviceSize bytes = VkDeviceSize(index_count) * (is32 ? 4 : 2);
      const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
      VkDeviceSize guest_offset;
      uint8_t* dst = src ? Upload(bytes, 16, guest_offset) : nullptr;
      if (!dst) {
        ++stats_.draws_failed;
        return true;
      }
      std::memcpy(dst, src, bytes);
      index_load_flags = SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad |
                         (is32 ? SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit : 0);
      index_load_address = uint32_t(guest_offset);
    }
    const uint32_t out_count = 4 * rects + (rects - 1);
    uint32_t* out = reinterpret_cast<uint32_t*>(Upload(VkDeviceSize(out_count) * 4, 16, index_offset));
    if (!out) {
      ++stats_.draws_failed;
      return true;
    }
    for (uint32_t r = 0, o = 0; r < rects; ++r) {
      if (r) out[o++] = UINT32_MAX;
      for (uint32_t j = 0; j < 4; ++j) out[o++] = (r << 2) + j;
    }
    index_type = VK_INDEX_TYPE_UINT32;
    use_indices = true;
    draw_count = out_count;
    if (!index_load_flags) index_endian = xenos::Endian::kNone;
  } else if (prim_type == xenos::PrimitiveType::kQuadList) {
    const uint32_t per = 4;
    const uint32_t prims = index_count / per;
    const uint32_t out_count = prims * 6;
    if (!prims) {
      ++stats_.draws_skipped;
      return true;
    }
    std::vector<uint32_t> guest_idx(index_count);
    if (indexed) {
      const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
      for (uint32_t i = 0; i < index_count; ++i) {
        if (index_type == VK_INDEX_TYPE_UINT32) {
          uint32_t v;
          std::memcpy(&v, src + 4 * i, 4);
          guest_idx[i] = v;  // kept in guest order, the shader swaps
        } else {
          uint16_t v;
          std::memcpy(&v, src + 2 * i, 2);
          guest_idx[i] = v;
        }
      }
    } else {
      for (uint32_t i = 0; i < index_count; ++i) guest_idx[i] = i;
    }
    uint32_t* out = reinterpret_cast<uint32_t*>(Upload(VkDeviceSize(out_count) * 4, 16, index_offset));
    if (!out) {
      ++stats_.draws_failed;
      return true;
    }
    for (uint32_t p = 0; p < prims; ++p) {
      const uint32_t* q = &guest_idx[p * per];
      const uint32_t tri[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
      std::memcpy(out + 6 * p, tri, sizeof(tri));
    }
    index_type = VK_INDEX_TYPE_UINT32;
    if (!indexed) index_endian = xenos::Endian::kNone;
    // 16-bit guest indices widened to 32 bits keep their 16-bit swap.
    use_indices = true;
    draw_count = out_count;
  } else if (indexed && native_vertex) {
    const VkDeviceSize bytes = VkDeviceSize(native_indices_.size()) * sizeof(uint32_t);
    if (!UploadImmutable(native_indices_.data(), size_t(bytes), 16, 100, index_offset)) {
      ++stats_.draws_failed; return true;
    }
    index_type = VK_INDEX_TYPE_UINT32;
    index_endian = xenos::Endian::kNone;
  } else if (indexed) {
    const VkDeviceSize bytes = index_buffer_info->length
                                   ? VkDeviceSize(index_buffer_info->length)
                                   : VkDeviceSize(index_count) * (index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2);
    const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
    if (!UploadImmutable(src, size_t(bytes), 16, 101, index_offset)) {
      ++stats_.draws_failed;
      return true;
    }
  }

  stats_.us_geometry += NowUs() - geometry_start;
  // System constants.
  SpirvShaderTranslator::SystemConstants& sc = system_constants_;
  std::memset(&sc, 0, sizeof(sc));
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  uint32_t flags = 0;
  if (vte.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  const xenos::CompareFunction alpha_func =
      color_control.alpha_test_enable ? color_control.alpha_func : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_func) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < last_used_rt; ++i) {
    const auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    if (REXCVAR_GET(ds2_native_clamp_7e3) &&
        (ci.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
         ci.color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
      flags |= SpirvShaderTranslator::kSysFlag_ClampColor0To7e3 << i;
    }
  }
  flags |= index_load_flags;
  sc.flags = flags;
  sc.vertex_index_load_address = index_load_address;
  sc.vertex_index_endian = index_endian;
  sc.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  sc.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  sc.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  draw_util::ViewportInfo viewport;
  // As the emulation: host viewport limits, reverse depth allowed (Vulkan accepts minDepth > maxDepth).
  draw_util::GetHostViewportInfo(regs, sx, sy, false, device()->properties().maxViewportDimensions[0],
                                 device()->properties().maxViewportDimensions[1], true, depth_control, false,
                                 REXCVAR_GET(ds2_native_float24_half_range),
                                 ps && ps->writes_depth(), viewport);
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport.ndc_scale[i];
    sc.ndc_offset[i] = viewport.ndc_offset[i];
  }
  sc.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  if (prim_type == xenos::PrimitiveType::kPointList) {
    const auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sc.point_screen_diameter_to_ndc_radius[0] = float(sx) / float(std::max(viewport.xy_extent[0], 1u));
    sc.point_screen_diameter_to_ndc_radius[1] = float(sy) / float(std::max(viewport.xy_extent[1], 1u));
  }
  for (uint32_t i = 0; i < 4; ++i) {
    const auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    int32_t bias = ci.color_exp_bias;
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        ci.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      bias -= 5;
    }
    const uint32_t bits = UINT32_C(0x3F800000) + uint32_t(bias << 23);
    std::memcpy(&sc.color_exp_bias[i], &bits, sizeof(float));
  }
  auto signs_of = [&](const std::vector<VulkanShader::TextureBinding>& tb) {
    for (const auto& b : tb) {
      const uint32_t i = b.fetch_constant;
      const uint8_t signs = texture_util::SwizzleSigns(regs.GetTextureFetch(i));
      sc.texture_swizzled_signs[i >> 2] |= uint32_t(signs) << (8 * (i & 3));
    }
  };
  signs_of(vs_textures);
  signs_of(ps_textures);

  // Constant buffers.
  const uint64_t constants_start = NowUs();
  const VkDeviceSize ub_align = std::max<VkDeviceSize>(device()->properties().minUniformBufferOffsetAlignment, 16);
  VkDescriptorBufferInfo cbuf[SpirvShaderTranslator::kConstantBufferCount];
  const bool reuse_constants = REXCVAR_GET(ds2_native_constant_reuse);
  auto put = [&](uint32_t index, const void* data, size_t size) -> bool {
    // RC77+: the same bytes as the previous draw's buffer of this kind (in this upload buffer) reuse its upload.
    LastConstantUpload& last = last_constant_uploads_[index];
    if (reuse_constants && last.frame == frame_ && last.data.size() == size &&
        std::memcmp(last.data.data(), data, size) == 0) {
      cbuf[index] = {slot().upload, last.offset, size};
      ++stats_.constant_uploads_reused;
      return true;
    }
    VkDeviceSize off;
    if (!UploadImmutable(data, size, ub_align, index, off)) return false;
    cbuf[index] = {slot().upload, off, size};
    if (reuse_constants) {
      last.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
      last.offset = off;
      last.frame = frame_;
    }
    return true;
  };
  auto put_float = [&](uint32_t index, const Shader& shader, uint32_t first_register) -> bool {
    const auto& map = shader.constant_register_map();
    const size_t size = sizeof(float) * 4 * std::max(map.float_count, 1u);
    alignas(16) std::array<float, 256 * 4> packed;
    if (size > sizeof(packed)) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(packed.data());
    uint8_t* w = p;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map.float_bitmap[i];
      uint32_t c;
      while (rex::bit_scan_forward(bits, &c)) {
        bits &= ~(uint64_t(1) << c);
        std::memcpy(w, &regs[first_register + (i << 8) + (c << 2)], sizeof(float) * 4);
        if (halfres_draw_ && halfres_mode_ == 1 && first_register == XE_GPU_REG_SHADER_CONSTANT_256_X &&
            int32_t(i * 64 + c) == REXCVAR_GET(ds2_native_halfres_vpos_const)) {
          // RC82: the pixel position is half of the guest one; its scale to texture coordinates is doubled.
          float* f = reinterpret_cast<float*>(w);
          f[2] *= float(halfres_divisor_);
          f[3] *= float(halfres_divisor_);
        }
        w += sizeof(float) * 4;
      }
    }
    if (w == p) std::memset(p, 0, 16);
    return put(index, p, size);
  };
  bool ok = put(SpirvShaderTranslator::kConstantBufferSystem, &sc, sizeof(sc)) &&
            put_float(SpirvShaderTranslator::kConstantBufferFloatVertex, *vs, XE_GPU_REG_SHADER_CONSTANT_000_X);
  if (ok) {
    if (ps) {
      ok = put_float(SpirvShaderTranslator::kConstantBufferFloatPixel, *ps, XE_GPU_REG_SHADER_CONSTANT_256_X);
    } else {
      static const float zero[4] = {};
      ok = put(SpirvShaderTranslator::kConstantBufferFloatPixel, zero, sizeof(zero));
    }
  }
  ok = ok && put(SpirvShaderTranslator::kConstantBufferBoolLoop, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                 sizeof(uint32_t) * (8 + 32));
  ok = ok && put(SpirvShaderTranslator::kConstantBufferFetch, fetch_constants, sizeof(fetch_constants));
  if (!ok) {
    ++stats_.draws_failed;
    return true;
  }
  // An upload that filled the buffer submitted the frame: the offsets above would mix two buffers. Drop the draw.
  if (frame_ != draw_frame) {
    ++stats_.draws_failed;
    return true;
  }
  stats_.us_constants += NowUs() - constants_start;
  const uint64_t descriptors_start = NowUs();
  const VkDevice dev = vulkan_device_->device();
  VkDescriptorSet sets[4] = {slot().shared_memory_set};
  const bool reuse_texture_sets = REXCVAR_GET(ds2_native_texture_sets);
  const bool persistent_sets = reuse_texture_sets && REXCVAR_GET(ds2_native_persistent_texture_sets);
  const bool dynamic_ubo = REXCVAR_GET(ds2_native_dynamic_ubo);
  std::array<VkDeviceSize, SpirvShaderTranslator::kConstantBufferCount> constant_ranges;
  uint32_t constant_offsets[SpirvShaderTranslator::kConstantBufferCount];
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_ranges[i] = cbuf[i].range;
    constant_offsets[i] = uint32_t(cbuf[i].offset);
  }
  bool write_constants = true;
  if (dynamic_ubo) {
    auto found = slot().constant_sets.find(constant_ranges);
    if (found != slot().constant_sets.end()) {
      sets[1] = found->second;
      write_constants = false;
    }
  }
  VkDescriptorSetLayout set_layouts[3] = {set_layout_constants_, layout->textures_vertex, layout->textures_pixel};
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = slot().descriptor_pool;
  ai.descriptorSetCount = (reuse_texture_sets || dynamic_ubo) ? 1 : 3;
  ai.pSetLayouts = set_layouts;
  if (write_constants && dfn().vkAllocateDescriptorSets(dev, &ai, sets + 1) != VK_SUCCESS) {
    ++stats_.draws_failed;
    return true;
  }
  auto& writes = descriptor_writes_;  // RC81: no allocation per draw
  writes.clear();
  writes.reserve(SpirvShaderTranslator::kConstantBufferCount + image_infos.size());
  if (write_constants) {
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      if (dynamic_ubo) cbuf[i].offset = 0;
      VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = sets[1];
      w.dstBinding = i;
      w.descriptorCount = 1;
      w.descriptorType = dynamic_ubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w.pBufferInfo = &cbuf[i];
      writes.push_back(w);
    }
  }
  size_t img_i = 0;
  std::vector<std::vector<uint64_t>> new_persistent;
  auto texture_set = [&](VkDescriptorSetLayout set_layout, size_t textures, size_t samplers,
                         VkDescriptorSet& set) -> bool {
    const size_t count = textures + samplers;
    std::vector<uint64_t> local_key;
    auto& key = (REXCVAR_GET(ds2_native_cpu_cache) || persistent_sets) ? descriptor_key_ : local_key;
    key.clear();
    if (persistent_sets) {
      key.reserve(1 + count * 3);
      key.push_back(reinterpret_cast<uint64_t>(set_layout));
      for (size_t i = 0; i < count; ++i) {
        const auto& info = image_infos[img_i + i];
        key.push_back(reinterpret_cast<uint64_t>(info.imageView));
        key.push_back(reinterpret_cast<uint64_t>(info.sampler));
        key.push_back(uint64_t(info.imageLayout));
      }
      auto found = persistent_texture_sets_.find(key);
      if (found != persistent_texture_sets_.end()) {
        set = found->second.set;
        img_i += count;
        ++stats_.texture_sets_reused;
        ++stats_.texture_sets_kept;
        return true;
      }
      PersistentSet ps;
      if (AllocatePersistentSet(set_layout, ps)) {
        set = ps.set;
        for (size_t i = 0; i < count; ++i) {
          VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          w.dstSet = set;
          w.dstBinding = uint32_t(i);
          w.descriptorCount = 1;
          w.descriptorType = i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
          w.pImageInfo = &image_infos[img_i + i];
          writes.push_back(w);
          if (i < textures && image_infos[img_i + i].imageView) {
            auto& keys = persistent_sets_by_view_[image_infos[img_i + i].imageView];
            if (keys.empty() || keys.back() != key) keys.push_back(key);
          }
        }
        img_i += count;
        persistent_texture_sets_.emplace(key, ps);
        new_persistent.push_back(key);
        ++stats_.texture_sets_created;
        return true;
      }
      // Pools full: per-frame set below.
    }
    key.clear();
    if (reuse_texture_sets) {
      key.reserve(1 + count * 3);
      key.push_back(reinterpret_cast<uint64_t>(set_layout));
      for (size_t i = 0; i < count; ++i) {
        const auto& info = image_infos[img_i + i];
        key.push_back(reinterpret_cast<uint64_t>(info.imageView));
        key.push_back(reinterpret_cast<uint64_t>(info.sampler));
        key.push_back(uint64_t(info.imageLayout));
      }
      auto found = slot().texture_sets.find(key);
      if (found != slot().texture_sets.end()) {
        set = found->second;
        img_i += count;
        ++stats_.texture_sets_reused;
        return true;
      }
    }
    ai.pSetLayouts = &set_layout;
    if ((reuse_texture_sets || dynamic_ubo) && dfn().vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return false;
    for (size_t i = 0; i < count; ++i) {
      VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = uint32_t(i);
      w.descriptorCount = 1;
      w.descriptorType = i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
      w.pImageInfo = &image_infos[img_i++];
      writes.push_back(w);
    }
    if (reuse_texture_sets) slot().texture_sets.emplace(key, set);
    ++stats_.texture_sets_created;
    return true;
  };
  if (!texture_set(layout->textures_vertex, vs_textures.size(), vs_samplers.size(), sets[2]) ||
      !texture_set(layout->textures_pixel, ps_textures.size(), ps_samplers.size(), sets[3])) {
    // A failed allocation must not leave cached sets whose writes were never applied.
    slot().texture_sets.clear();
    for (const auto& k : new_persistent) {
      auto pit = persistent_texture_sets_.find(k);
      if (pit == persistent_texture_sets_.end()) continue;
      free_descriptor_sets_(dev, pit->second.pool, 1, &pit->second.set);
      persistent_texture_sets_.erase(pit);
    }
    ++stats_.draws_failed;
    return true;
  }
  if (!writes.empty()) dfn().vkUpdateDescriptorSets(dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
  if (dynamic_ubo && write_constants) slot().constant_sets.emplace(constant_ranges, sets[1]);
  if (write_constants) ++stats_.constant_sets_created;
  else ++stats_.constant_sets_reused;
  stats_.us_descriptors += NowUs() - descriptors_start;

  // RC74: shadow cube faces. A depth-only clear rectangle is remembered; the next depth-only draw on the same
  // target with a viewport smaller than the target clears that viewport to the clear depth first (DS2 clears the
  // faces with a 1x and a 4x rectangle; parts of the face kept depth from earlier lights: squares and stripes in
  // the torch light).
  if (REXCVAR_GET(ds2_native_shadow_face_clear) && !color_count && depth_rt && !host_msaa) {
    const bool clear_rect = prim_type == xenos::PrimitiveType::kRectangleList && depth_control.z_enable &&
                            depth_control.z_write_enable && depth_control.zfunc == xenos::CompareFunction::kAlways;
    if (clear_rect) {
      if (!std::isnan(rect_first_z)) {
        pending_face_clear_.image = depth_rt->image;
        pending_face_clear_.depth = std::clamp(rect_first_z, 0.0f, 1.0f);
      }
    } else if (pending_face_clear_.image == depth_rt->image && depth_control.z_enable) {
      const int32_t vx = std::max<int32_t>(viewport.xy_offset[0], 0), vy = std::max<int32_t>(viewport.xy_offset[1], 0);
      const uint32_t vw = std::min<uint32_t>(viewport.xy_extent[0], depth_rt->width > uint32_t(vx) ? depth_rt->width - vx : 0);
      const uint32_t vh = std::min<uint32_t>(viewport.xy_extent[1], depth_rt->height > uint32_t(vy) ? depth_rt->height - vy : 0);
      if (vw && vh && vw * 2 <= depth_rt->width) {  // a face of a packed target, not a full-screen pass
        VkClearValue cv = {};
        cv.depthStencil.depth = pending_face_clear_.depth;
        ClearRect(*depth_rt, cv, {{vx, vy}, {vw, vh}}, true);
        ++stats_.face_clears;
        if (Tracing()) {
          REXGPU_INFO("[native trace]   shadow face clear {},{} {}x{} depth {}", vx, vy, vw, vh,
                      pending_face_clear_.depth);
        }
      }
      pending_face_clear_.image = VK_NULL_HANDLE;
    }
  } else if (color_count) {
    pending_face_clear_.image = VK_NULL_HANDLE;
  }

  // Render pass.
  ScopedUs commands_time(stats_.us_commands);
  uint64_t fb_key = reinterpret_cast<uint64_t>(pass);
  for (uint32_t i = 0; i < color_count; ++i) fb_key = fb_key * 1000003u + reinterpret_cast<uint64_t>(color_rts[i]->view);
  if (depth_rt) fb_key = fb_key * 1000003u + reinterpret_cast<uint64_t>(depth_rt->view);
  fb_key = fb_key * 31 + rt_width * 4099 + rt_height;
  VkCommandBuffer cmd = slot().cmd;
  if (!in_render_pass_ || current_framebuffer_key_ != fb_key) {
    EndRenderPass();
    VkFramebuffer fb;
    auto fit = framebuffers_.find(fb_key);
    if (fit != framebuffers_.end()) {
      fb = fit->second;
    } else {
      VkImageView views[5];
      uint32_t n = 0;
      for (uint32_t i = 0; i < color_count; ++i) views[n++] = color_rts[i]->view;
      if (depth_rt) views[n++] = depth_rt->view;
      VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fci.renderPass = pass;
      fci.attachmentCount = n;
      fci.pAttachments = views;
      fci.width = rt_width;
      fci.height = rt_height;
      fci.layers = 1;
      if (dfn().vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) {
        ++stats_.draws_failed;
        return true;
      }
      framebuffers_.emplace(fb_key, fb);
      for (uint32_t i = 0; i < n; ++i) fb_by_view_.emplace(views[i], fb_key);
    }
    // v55: no full barrier between two render passes. Every copy, clear, conversion and upload already ends
    // with one, and textures never sample a render target directly (resolves copy it), so only attachment
    // after attachment remains, which the GPU keeps in order. Each NVK barrier is a wait-for-idle plus cache
    // flushes (~50 per frame in DS2).
    if (REXCVAR_GET(ds2_native_pass_barriers)) Barrier();
    if (profiling_) {
      VkFormat cf[4];
      for (uint32_t i = 0; i < color_count; ++i) cf[i] = color_rts[i]->format;
      MarkPass(cf, color_count, depth_rt ? depth_rt->format : VK_FORMAT_UNDEFINED, rt_width, rt_height);
    }
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {rt_width, rt_height}};
    work_since_barrier_ = true;
    InvalidateDrawState();
    for (uint32_t i = 0; i < color_count; ++i) Written(*color_rts[i]);
    if (depth_rt) Written(*depth_rt);
    dfn().vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    in_render_pass_ = true;
    if (Tracing()) {
      std::string rts;
      for (uint32_t i = 0; i < color_count; ++i) {
        const auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
        rts += fmt::format(" C{}@{}", uint32_t(ci.color_format), ci.color_base);
      }
      if (depth_rt) {
        const auto di = regs.Get<reg::RB_DEPTH_INFO>();
        rts += fmt::format(" D{}@{}", uint32_t(di.depth_format), di.depth_base);
      }
      REXGPU_INFO("[native trace] draw {} pass{} pitch {} msaa {} {}x{} vs {:016X} ps {:016X}", draws_this_frame_, rts,
                  surface_info.surface_pitch, uint32_t(surface_info.msaa_samples), rt_width, rt_height,
                  vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0);
    }
    current_framebuffer_key_ = fb_key;
    current_render_pass_ = pass;
    current_rt_width_ = rt_width;
    current_rt_height_ = rt_height;
    ++stats_.render_passes_begun;
  }

  if (Tracing() && !DumpDir().empty()) {
    // Shader disassembly of the traced frame, one file per shader.
    for (VulkanShader* sh : {vs, ps}) {
      if (!sh) continue;
      const std::string path = fmt::format("{}/shader_{:016X}_{}.txt", DumpDir(),
                                           sh->ucode_data_hash(), sh->type() == xenos::ShaderType::kVertex ? "vs" : "ps");
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(sh->ucode_disassembly().data(), 1, sh->ucode_disassembly().size(), f);
        std::fclose(f);
      }
    }
  }
  if (Tracing()) {
    REXGPU_INFO("[native trace]   draw {} prim {} n {} idx {} z(en {} wr {} fn {}) st {} vp {},{} {}x{} z {:.3f}..{:.3f} "
                "ndcz {:.3f}+{:.3f} cull {} mask {:X} vte {:X} clip {:X} blend {:08X} ps {:016X} sref {:08X}",
                draws_this_frame_, uint32_t(prim_type), index_count, indexed ? 1 : 0, uint32_t(depth_control.z_enable),
                uint32_t(depth_control.z_write_enable), uint32_t(depth_control.zfunc),
                uint32_t(depth_control.stencil_enable), viewport.xy_offset[0], viewport.xy_offset[1],
                viewport.xy_extent[0], viewport.xy_extent[1], viewport.z_min, viewport.z_max, viewport.ndc_scale[2],
                viewport.ndc_offset[2], key.cull_mode, write_mask, regs[XE_GPU_REG_PA_CL_VTE_CNTL],
                regs[XE_GPU_REG_PA_CL_CLIP_CNTL], key.blend[0], ps ? ps->ucode_data_hash() : 0,
                regs[XE_GPU_REG_RB_STENCILREFMASK]);
  }
  const uint32_t shader_profile_every = uint32_t(REXCVAR_GET(ds2_native_shader_profile_every));
  const bool sample_shader = profiling_ &&
                             ((shader_profile_every && (profile_counter_ - 1) % shader_profile_every == 0) ||
                              spike_frames_ > 0) &&
                             color_count && viewport.xy_extent[0] >= 640 && viewport.xy_extent[1] >= 360;
  if (!sample_shader && profile_shader_active_) {
    Mark("other (outside scene shader group)");
    profile_shader_active_ = false;
    profile_shader_pipeline_ = VK_NULL_HANDLE;
  }
  const uint32_t shader_phase = color_count && (color_formats[0] == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                                color_formats[0] == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
                                    ? 1 : (color_count && depth_rt ? 0 : 2);
  uint32_t phase_limits[3] = {64, 96, 32};
  if (const int32_t g = REXCVAR_GET(ds2_native_shader_profile_groups); g > 0) {
    phase_limits[0] = phase_limits[1] = phase_limits[2] = uint32_t(g);
  }
  if (sample_shader && pipeline != profile_shader_pipeline_) {
    if (profile_shader_phase_groups_[shader_phase] < phase_limits[shader_phase]) {
      if (spike_frames_ > 0) {
        // RC81: in a spike window the group name also says what the draw is (size, blending, textures).
        std::string tex;
        for (const auto& t : ps_textures) {
          const auto f = regs.GetTextureFetch(t.fetch_constant);
          tex += fmt::format(" t{}:f{} {}x{} mip{}-{} mag{} min{} an{}", t.fetch_constant, uint32_t(f.format),
                             f.size_2d.width + 1, f.size_2d.height + 1, uint32_t(f.mip_min_level),
                             uint32_t(f.mip_max_level), uint32_t(f.mag_filter), uint32_t(f.min_filter),
                             uint32_t(f.aniso_filter));
        }
        Mark(fmt::format("shader VS {:016X} PS {:016X} | prim {} n {} vp {}x{} blend {:08X} z {}{}{} early {} |{}",
                         key.vs_hash, key.ps_hash, uint32_t(prim_type), index_count, viewport.xy_extent[0],
                         viewport.xy_extent[1], key.blend[0], uint32_t(depth_control.z_enable),
                         uint32_t(depth_control.z_write_enable), uint32_t(depth_control.zfunc),
                         ps ? uint32_t(ps_mod.pixel.depth_stencil_mode) : 0u, tex));
      } else {
        Mark(fmt::format("shader VS {:016X} PS {:016X}", key.vs_hash, key.ps_hash));
      }
      profile_shader_pipeline_ = pipeline;
      profile_shader_active_ = true;
      ++profile_shader_groups_;
      ++profile_shader_phase_groups_[shader_phase];
    } else if (profile_shader_active_) {
      Mark("other (shader sampling limit)");
      profile_shader_active_ = false;
    }
  }
  if (native_vertex) {
    VkBuffer buffers[8];
    std::fill_n(buffers, native_plan.bindings.size(), slot().upload);
    dfn().vkCmdBindVertexBuffers(cmd, 0, uint32_t(native_plan.bindings.size()), buffers, native_vertex_offsets);
    ++stats_.native_vertex_draws;
  } else {
    ++stats_.legacy_vertex_draws;
  }
  // RC68: redundant state calls are skipped (ds2_native_state_filter); the cache is reset with every command
  // buffer, render pass and foreign pipeline bind.
  DrawStateCache& ds = draw_state_;
  const bool filter = REXCVAR_GET(ds2_native_state_filter) && ds.valid;
  const uint32_t offset_count = dynamic_ubo ? uint32_t(SpirvShaderTranslator::kConstantBufferCount) : 0;
  if (!filter || ds.pipeline != pipeline) {
    dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    ds.pipeline = pipeline;
    ++stats_.state_calls;
  } else {
    ++stats_.state_calls_skipped;
  }
  if (!filter || ds.layout != layout->layout || std::memcmp(ds.sets, sets, sizeof(ds.sets)) ||
      ds.offset_count != offset_count ||
      (offset_count && std::memcmp(ds.offsets, constant_offsets, sizeof(uint32_t) * offset_count))) {
    dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, 0, 4, sets, offset_count,
                                  offset_count ? constant_offsets : nullptr);
    ds.layout = layout->layout;
    std::memcpy(ds.sets, sets, sizeof(ds.sets));
    ds.offset_count = offset_count;
    if (offset_count) std::memcpy(ds.offsets, constant_offsets, sizeof(uint32_t) * offset_count);
    ++stats_.state_calls;
  } else {
    ++stats_.state_calls_skipped;
  }
  VkViewport vp;
  vp.x = float(viewport.xy_offset[0]);
  vp.y = float(viewport.xy_offset[1]);
  vp.width = float(std::max(viewport.xy_extent[0], 1u));
  vp.height = float(std::max(viewport.xy_extent[1], 1u));
  if (draw_scaled) {
    vp.x *= res_scale_;
    vp.y *= res_scale_;
    vp.width *= res_scale_;
    vp.height *= res_scale_;
  }
  const draw_util::Scissor full_scissor = scissor;
  if (halfres_draw_) {
    const float inv = 1.0f / float(halfres_divisor_);
    vp.x *= inv;
    vp.y *= inv;
    vp.width *= inv;
    vp.height *= inv;
    scissor.offset[0] /= halfres_divisor_;
    scissor.offset[1] /= halfres_divisor_;
    scissor.extent[0] = (scissor.extent[0] + halfres_divisor_ - 1) / halfres_divisor_;
    scissor.extent[1] = (scissor.extent[1] + halfres_divisor_ - 1) / halfres_divisor_;
  }
  vp.minDepth = viewport.z_min;
  vp.maxDepth = viewport.z_max;
  if (!filter || std::memcmp(&ds.viewport, &vp, sizeof(vp))) {
    dfn().vkCmdSetViewport(cmd, 0, 1, &vp);
    ds.viewport = vp;
    ++stats_.state_calls;
  } else {
    ++stats_.state_calls_skipped;
  }
  VkRect2D sr;
  sr.offset.x = int32_t(std::min(scissor.offset[0], rt_width));
  sr.offset.y = int32_t(std::min(scissor.offset[1], rt_height));
  sr.extent.width = std::min(scissor.extent[0], rt_width - uint32_t(sr.offset.x));
  sr.extent.height = std::min(scissor.extent[1], rt_height - uint32_t(sr.offset.y));
  if (probe_active_ == 1) sr.extent = {0, 0};  // RC90 probe: no fragments
  {
    // v109: the rows this draw can write (EDRAM ownership, see NativeImage::epoch_rows).
    uint32_t bottom = uint32_t(sr.offset.y) + sr.extent.height;
    const float vp_bottom = vp.y + vp.height;
    if (vp_bottom > 0.0f) bottom = std::min(bottom, uint32_t(std::ceil(vp_bottom)));
    if (!std::isnan(rect_max_y)) {
      // Window coordinates (no y scale or offset): the rectangle's own extent.
      float y = (rect_max_y + 0.5f) * float(sy);
      if (draw_scaled) y *= res_scale_;
      if (halfres_draw_) y /= float(halfres_divisor_);
      bottom = std::min(bottom, uint32_t(std::max(std::ceil(y), 0.0f)));
    }
    for (uint32_t i = 0; i < color_count; ++i) {
      if ((write_mask >> (4 * i)) & 0xF) color_rts[i]->Wrote(bottom);
    }
    if (depth_rt && (depth_control.z_write_enable || depth_control.stencil_enable)) depth_rt->Wrote(bottom);
    if (Tracing()) {
      REXGPU_INFO("[native trace]   draw {} writes rows < {} (rectangle bottom {})", draws_this_frame_, bottom,
                  rect_max_y);
    }
  }
  if (!filter || std::memcmp(&ds.scissor, &sr, sizeof(sr))) {
    dfn().vkCmdSetScissor(cmd, 0, 1, &sr);
    ds.scissor = sr;
    ++stats_.state_calls;
  } else {
    ++stats_.state_calls_skipped;
  }
  float bias_const = 0.0f, bias_slope = 0.0f;
  const bool bias_absolute = cmd_set_depth_bias2_ && REXCVAR_GET(ds2_native_depth_bias_absolute);
  if (bias_absolute) {
    // RC76: the face that is drawn (front, or back when front faces are culled), and the line/point offset, as the
    // emulation selects it. The Xenos constant offset is absolute (Direct3D 9). The host float32 depth would take
    // a constant factor relative to the exponent of the primitive's depth, which for reversed depth near 0 (DS2
    // point-light cube faces) makes the bias up to hundreds of times smaller: shadow acne, striped squares on
    // torch-lit walls. With the unorm24 representation, factor * 2^-24 is the guest offset itself.
    float scale = 0.0f, offset = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, polygonal, scale, offset);
    bias_slope = scale * xenos::kPolygonOffsetScaleSubpixelUnit;
    if (REXCVAR_GET(ds2_native_bias_sample_scale)) bias_slope *= float(std::max(sx, sy));
    bias_const = offset * float(1 << 24);
  } else if (polygonal && (sc_mode.poly_offset_front_enable || sc_mode.poly_offset_back_enable)) {
    bias_slope = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE) * xenos::kPolygonOffsetScaleSubpixelUnit;
    // RC72: a 4x target drawn in sample space has twice the pixels per axis, so depth changes half as much per
    // pixel and the slope-scaled bias would be halved (shadow acne: stripes in the torch light).
    if (REXCVAR_GET(ds2_native_bias_sample_scale)) bias_slope *= float(std::max(sx, sy));
    bias_const = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET) * float(1 << 24);
  }
  const float bias[2] = {bias_const, bias_slope};
  if (!filter || std::memcmp(ds.bias, bias, sizeof(bias))) {
    if (bias_absolute) {
      VkDepthBiasRepresentationInfoEXT rep = {VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT};
      rep.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
      rep.depthBiasExact = VK_FALSE;
      VkDepthBiasInfoEXT bi = {VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT, &rep};
      bi.depthBiasConstantFactor = bias_const;
      bi.depthBiasClamp = 0.0f;
      bi.depthBiasSlopeFactor = bias_slope;
      cmd_set_depth_bias2_(cmd, &bi);
    } else {
      dfn().vkCmdSetDepthBias(cmd, bias_const, 0.0f, bias_slope);
    }
    std::memcpy(ds.bias, bias, sizeof(bias));
    ++stats_.state_calls;
  } else {
    ++stats_.state_calls_skipped;
  }
  const float blend_constants[4] = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                                    regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
                                    regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (!filter || std::memcmp(ds.blend, blend_constants, sizeof(blend_constants))) {
    dfn().vkCmdSetBlendConstants(cmd, blend_constants);
    std::memcpy(ds.blend, blend_constants, sizeof(blend_constants));
    ++stats_.state_calls;
  } else {
    ++stats_.state_calls_skipped;
  }
  const auto srm = regs.Get<reg::RB_STENCILREFMASK>();
  const auto srm_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  const bool bf = polygonal && depth_control.backface_enable;
  const uint32_t stencil[6] = {srm.stencilmask, bf ? srm_bf.stencilmask : srm.stencilmask, srm.stencilwritemask,
                               bf ? srm_bf.stencilwritemask : srm.stencilwritemask, srm.stencilref,
                               bf ? srm_bf.stencilref : srm.stencilref};
  if (!filter || std::memcmp(ds.stencil, stencil, sizeof(stencil))) {
    dfn().vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[0]);
    dfn().vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[1]);
    dfn().vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[2]);
    dfn().vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[3]);
    dfn().vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[4]);
    dfn().vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[5]);
    std::memcpy(ds.stencil, stencil, sizeof(stencil));
    ++stats_.state_calls;
  } else {
    ++stats_.state_calls_skipped;
  }
  ds.valid = true;
  if (probe_active_ == 2) draw_count = std::min<uint32_t>(draw_count, 3);  // RC90 probe: one triangle
  if (use_indices) {
    dfn().vkCmdBindIndexBuffer(cmd, slot().upload, index_offset, index_type);
    work_since_barrier_ = true;
    dfn().vkCmdDrawIndexed(cmd, draw_count, 1, 0, 0, 0);
  } else {
    work_since_barrier_ = true;
    dfn().vkCmdDraw(cmd, draw_count, 1, 0, 0);
  }
  ++stats_.draws;
  ++draws_this_frame_;
  if (profiling_ && !slot().seg_draws.empty()) ++slot().seg_draws.back();
  if (halfres_draw_ && halfres_full_rt) {
    halfres_draw_ = false;
    const uint32_t fx = std::min(full_scissor.offset[0], halfres_full_rt->width);
    const uint32_t fy = std::min(full_scissor.offset[1], halfres_full_rt->height);
    const uint32_t fx1 = fx + std::min(full_scissor.extent[0], halfres_full_rt->width - fx);
    const uint32_t fy1 = fy + std::min(full_scissor.extent[1], halfres_full_rt->height - fy);
    VkRect2D& u = halfres_pending_rect_;
    if (!u.extent.width || !u.extent.height) {
      u = {{int32_t(fx), int32_t(fy)}, {fx1 - fx, fy1 - fy}};
    } else {
      const uint32_t ux1 = uint32_t(u.offset.x) + u.extent.width, uy1 = uint32_t(u.offset.y) + u.extent.height;
      const uint32_t nx = std::min<uint32_t>(uint32_t(u.offset.x), fx), ny = std::min<uint32_t>(uint32_t(u.offset.y), fy);
      u = {{int32_t(nx), int32_t(ny)}, {std::max(ux1, fx1) - nx, std::max(uy1, fy1) - ny}};
    }
    ++stats_.halfres_draws;
  }
  if (msaa_replay_clear_ && !msaa_force_clear_) {
    msaa_replay_clear_ = false;
    msaa_force_clear_ = true;
    IssueDraw(prim_type, index_count, index_buffer_info, major_mode_explicit);
    msaa_force_clear_ = false;
  }
  msaa_replay_clear_ = false;
  return true;
}

// --- Resolves ---------------------------------------------------------------------------------------------------

bool NativeCommandProcessor::ClearRect(NativeImage& rt, const VkClearValue& value, const VkRect2D& rect,
                                       bool depth_only) {
  Written(rt);
  rt.Wrote(uint32_t(rect.offset.y) + rect.extent.height);
  // A one-attachment render pass on the target and vkCmdClearAttachments for the rectangle.
  VkFormat color_format = rt.format;
  VkRenderPass pass = rt.depth ? GetRenderPass(nullptr, 0, rt.format) : GetRenderPass(&color_format, 1,
                                                                                       VK_FORMAT_UNDEFINED);
  if (!pass) return false;
  uint64_t fb_key = (reinterpret_cast<uint64_t>(pass) * 1000003u + reinterpret_cast<uint64_t>(rt.view)) * 31 +
                    rt.width * 4099 + rt.height;
  VkFramebuffer fb;
  auto fit = framebuffers_.find(fb_key);
  if (fit != framebuffers_.end()) {
    fb = fit->second;
  } else {
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &rt.view;
    fci.width = rt.width;
    fci.height = rt.height;
    fci.layers = 1;
    if (dfn().vkCreateFramebuffer(vulkan_device_->device(), &fci, nullptr, &fb) != VK_SUCCESS) return false;
    framebuffers_.emplace(fb_key, fb);
    fb_by_view_.emplace(rt.view, fb_key);
  }
  EndRenderPass();
  Barrier();
  if (profiling_) Mark(fmt::format("clear {} {}x{}", rt.depth ? "depth" : "color", rt.width, rt.height));
  VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = pass;
  bi.framebuffer = fb;
  bi.renderArea = {{0, 0}, {rt.width, rt.height}};
  VkCommandBuffer cmd = slot().cmd;
  work_since_barrier_ = true;
  InvalidateDrawState();
  dfn().vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  VkClearAttachment ca = {};
  ca.aspectMask = rt.depth ? (depth_only ? VK_IMAGE_ASPECT_DEPTH_BIT
                                         : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                           : VK_IMAGE_ASPECT_COLOR_BIT;
  ca.colorAttachment = 0;
  ca.clearValue = value;
  VkClearRect cr = {};
  cr.rect = rect;
  cr.layerCount = 1;
  work_since_barrier_ = true;
  dfn().vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
  dfn().vkCmdEndRenderPass(cmd);
  Barrier();
  ++stats_.clears;
  return true;
}

bool NativeCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                                                uint32_t count) {
  if (!REXCVAR_GET(ds2_native_occlusion_queries) || !vulkan_device_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }
  // Direct3D writes 0xFFFFFEED (big endian) to the result block on D3DISSUE_END and waits for the GPU to replace
  // it; the begin block is read back too and subtracted (begin = zeros here, as in the emulation).
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  const uint32_t address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
  if (!counts) return true;
  const bool is_end = counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished ||
                      counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished;
  std::memset(counts, 0, sizeof(*counts));
  if (!frame_open_ && !BeginFrame()) return true;
  FrameSlot& s = slot();
  if (!is_end) {
    ++stats_.occlusion_begins;
    if (occlusion_active_ >= 0) EndOcclusionQuery(0);  // unmatched begin: dropped
    if (s.occlusion_pool && s.occlusion_used < kMaxOcclusionQueries) {
      EndRenderPass();
      occlusion_active_ = int32_t(s.occlusion_used++);
      occlusion_unmeasured_ = false;
      dfn().vkCmdBeginQuery(s.cmd, s.occlusion_pool, uint32_t(occlusion_active_),
                            device()->properties().occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    }
    return true;
  }
  ++stats_.occlusion_ends;
  EndOcclusionQuery(address);
  // The newest measured result of this query (its memory is fixed per Direct3D query object); a query not
  // measured yet reports visible, as the emulation does.
  auto it = occlusion_results_.find(address);
  uint32_t samples = uint32_t(std::max(REXCVAR_GET(query_occlusion_fake_sample_count), 0));
  if (it != occlusion_results_.end()) {
    const OcclusionResult& r = it->second;
    const uint32_t hold = uint32_t(std::max(REXCVAR_GET(ds2_native_occlusion_hold), 1));
    samples = r.samples ? r.samples : (r.zero_streak >= hold ? 0 : r.max_recent);
  }
  if (samples) ++stats_.occlusion_visible;
  counts->ZPass_A = samples;
  counts->Total_A = samples;
  return true;
}

void NativeCommandProcessor::EndOcclusionQuery(uint32_t result_address) {
  if (occlusion_active_ < 0) return;
  EndRenderPass();
  FrameSlot& s = slot();
  dfn().vkCmdEndQuery(s.cmd, s.occlusion_pool, uint32_t(occlusion_active_));
  if (result_address) {
    if (occlusion_unmeasured_) {
      NoteOcclusionResult(result_address, uint32_t(std::max(REXCVAR_GET(query_occlusion_fake_sample_count), 1)));
    } else {
      s.occlusion_pending.emplace_back(result_address, uint32_t(occlusion_active_));
    }
  }
  occlusion_active_ = -1;
  occlusion_unmeasured_ = false;
}

bool NativeCommandProcessor::IssueCopy() {
  LatchRenderScale();
  FlushHalfRes();
  ScopedUs copy_timer(stats_.us_copies);
  const RegisterFile& regs = *register_file_;
  if (!frame_open_ && !BeginFrame()) return false;
  ++stats_.copies;
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) return true;
  // The rectangle in render target pixels (same computation as GetResolveInfo).
  const xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(0);
  const float* vertices = memory_->TranslatePhysical<const float*>(uint32_t(vfetch.address) << 2);
  if (!vertices) return true;
  float v[6];
  for (int i = 0; i < 6; ++i) v[i] = xenos::GpuSwap(vertices[i], vfetch.endian);
  int32_t x0 = int32_t(std::min({v[0], v[2], v[4]}) + 0.5f), y0 = int32_t(std::min({v[1], v[3], v[5]}) + 0.5f);
  int32_t x1 = int32_t(std::max({v[0], v[2], v[4]}) + 0.5f), y1 = int32_t(std::max({v[1], v[3], v[5]}) + 0.5f);
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += wo.window_x_offset;
    x1 += wo.window_x_offset;
    y0 += wo.window_y_offset;
    y1 += wo.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);
  x0 = std::clamp<int32_t>(x0, scissor.offset[0], scissor.offset[0] + scissor.extent[0]);
  x1 = std::clamp<int32_t>(x1, scissor.offset[0], scissor.offset[0] + scissor.extent[0]);
  y0 = std::clamp<int32_t>(y0, scissor.offset[1], scissor.offset[1] + scissor.extent[1]);
  y1 = std::clamp<int32_t>(y1, scissor.offset[1], scissor.offset[1] + scissor.extent[1]);
  if (x1 <= x0 || y1 <= y0) return true;

  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const int32_t sx = surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1;
  const int32_t sy = surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 2 : 1;
  const uint32_t sample_pitch = surface_info.surface_pitch * uint32_t(sx);
  // Guest pixels -> sample space.
  x0 *= sx;
  x1 *= sx;
  y0 *= sy;
  y1 *= sy;
  const bool is_depth = info.IsCopyingDepth();
  NativeImage* src = nullptr;
  if (is_depth) {
    const auto di = regs.Get<reg::RB_DEPTH_INFO>();
    src = GetRenderTarget(di.depth_base, sample_pitch, uint32_t(di.depth_format), true, uint32_t(y1));
  } else {
    const auto ci =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[info.rb_copy_control.copy_src_select]);
    src = GetRenderTarget(ci.color_base, sample_pitch, uint32_t(ci.color_format), false, uint32_t(y1));
  }
  if (!src) return true;
  // v107: a source at the reduced internal resolution: the rectangle (and the copy) in its pixels.
  const bool copy_scaled = src->scaled;
  const uint32_t guest_y1 = uint32_t(y1);
  if (copy_scaled) {
    x0 = Sc(x0);
    y0 = Sc(y0);
    x1 = Sc(x1);
    y1 = Sc(y1);
  }
  x1 = std::min<int32_t>(x1, int32_t(src->width));
  y1 = std::min<int32_t>(y1, int32_t(src->height));
  if (x1 <= x0 || y1 <= y0) return true;
  int32_t clear_x1 = x1, clear_y1 = y1;
  EndRenderPass();

  // Copy into the native texture of the destination address.
  const uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  const auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  const auto dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  if (dest_base && info.rb_copy_control.copy_command != xenos::CopyCommand::kNull) {
    // The resolved texture keeps the sample resolution of the source (sampled with normalized coordinates); a
    // resolve writes source pixel (x, y) to texel (x, y) of the destination.
    // RC70: a resolve rectangle is aligned to 8-pixel EDRAM tiles (320x184 for a 320x180 texture). Clipping the
    // copy to the destination size keeps the image at the texture size, so sampling it needs no extra
    // "logical size" copy (2 more copies and 4 barriers for every such step of the post-processing chains).
    // v107: destination sizes in the source's pixels (reduced internal resolution).
    const uint32_t dest_w = copy_scaled ? Sc(uint32_t(dest_pitch.copy_dest_pitch)) : uint32_t(dest_pitch.copy_dest_pitch);
    const uint32_t dest_h = copy_scaled ? Sc(uint32_t(dest_pitch.copy_dest_height)) : uint32_t(dest_pitch.copy_dest_height);
    const bool clip_to_dest = REXCVAR_GET(ds2_native_resolve_clip_to_dest) && sx == 1 && sy == 1 &&
                              dest_w && dest_h && x0 < int32_t(dest_w) && y0 < int32_t(dest_h);
    if (clip_to_dest) {
      x1 = std::min<int32_t>(x1, int32_t(dest_w));
      y1 = std::min<int32_t>(y1, int32_t(dest_h));
    }
    const uint32_t dw = std::max<uint32_t>(dest_w * uint32_t(sx), uint32_t(x1));
    const uint32_t dh = std::max<uint32_t>(dest_h * uint32_t(sy), uint32_t(y1));
    // Depth resolved into a color texture (DS2 reads its depth buffers as k_8_8_8_8 and rebuilds depth in the
    // shader): the resolved image holds the guest 32-bit words as RGBA8.
    const xenos::TextureFormat dest_tex_format = xenos::TextureFormat(dest_info.copy_dest_format);
    // Measured in DS2: depth is resolved with a k_8_8_8_8 destination format but sampled as k_24_8 /
    // k_24_8_FLOAT, so the resolved image stays a depth image (sampled through its depth aspect).
    const bool depth_as_color = false;
    NativeImage* copy_src = src;
    // Only the measured numeric path: 1x float RGBA -> unsigned RGBA8. Do not reinterpret
    // depth, gamma targets, other destination formats or MSAA sample selection here.
    const int32_t resolve_exp_bias = int32_t(info.copy_dest_info.copy_dest_exp_bias);
    if (REXCVAR_GET(ds2_native_resolve_color_convert) && !is_depth &&
        surface_info.msaa_samples == xenos::MsaaSamples::k1X &&
        (src->format == VK_FORMAT_R16G16B16A16_SFLOAT || src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) &&
        dest_tex_format == xenos::TextureFormat::k_8_8_8_8 &&
        dest_info.copy_dest_number == xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction) {
      const VkRect2D rect = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
      if (NativeImage* converted = PrepareColorResolve(*src, rect, resolve_exp_bias)) {
        copy_src = converted;
      }
    }
    const VkFormat resolved_format = depth_as_color ? VK_FORMAT_R8G8B8A8_UNORM : copy_src->format;
    // A resolve into a part of a bigger texture (DS2 packs 4 shadow cascades into one 1600x1600 atlas): Direct3D
    // moves the destination base to the 32x32 tile of the destination point. Find the texture that contains the
    // address and the tile it starts at.
    const FormatInfo* dest_fi = FormatInfo::Get(dest_tex_format);
    const uint32_t dest_bpp_log2 =
        is_depth ? 2u : rex::log2_floor(std::max<uint32_t>(dest_fi ? dest_fi->bits_per_pixel / 8 : 4, 1));
    uint32_t entry_base = dest_base;
    int32_t sub_x = 0, sub_y = 0;
    if (!resolved_.count(dest_base)) {
      for (auto& [eb, e] : resolved_) {
        if (eb >= dest_base || e.guest_pitch != dest_pitch.copy_dest_pitch || e.bpp_log2 != dest_bpp_log2) continue;
        const uint32_t pitch_aligned = rex::align<uint32_t>(e.guest_pitch, 32);
        const uint32_t height_aligned = rex::align<uint32_t>(e.guest_height, 32);
        if (dest_base - eb >= (pitch_aligned * height_aligned) << e.bpp_log2) continue;
        const int32_t delta = int32_t(dest_base - eb);
        bool found = false;
        for (uint32_t ty = 0; ty < height_aligned && !found; ty += 32) {
          for (uint32_t tx = 0; tx < pitch_aligned; tx += 32) {
            if (texture_util::GetTiledOffset2D(int32_t(tx), int32_t(ty), pitch_aligned, e.bpp_log2) == delta) {
              sub_x = int32_t(tx);
              sub_y = int32_t(ty);
              found = true;
              break;
            }
          }
        }
        if (found) {
          entry_base = eb;
          break;
        }
      }
    }
    const bool sub_resolve = entry_base != dest_base;
    ResolvedEntry& r = resolved_[entry_base];
    if (r.image.image && !sub_resolve &&
        (r.image.width < dw || r.image.height < dh || r.image.format != resolved_format)) {
      for (auto& [s, view] : r.swizzle_views) slot().deferred_views.push_back(view);
      r.swizzle_views.clear();
      DestroyResolveLogicalImages(r, true);
      DestroyImage(r.image, true);
    }
    if (!r.image.image) {
      VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (depth_as_color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
                                (src->depth && !depth_as_color ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
      if (!CreateImage(r.image, resolved_format, dw, dh, src->depth && !depth_as_color, usage)) {
        resolved_.erase(dest_base);
        return true;
      }
      EnsureGeneral(r.image);
    }
    if (!sub_resolve) {
      r.guest_format = uint32_t(dest_info.copy_dest_format);
      r.rb_swapped = !is_depth && dest_info.copy_dest_swap;
      r.guest_pitch = dest_pitch.copy_dest_pitch;
      r.guest_height = dest_pitch.copy_dest_height;
      r.bpp_log2 = dest_bpp_log2;
      r.sample_scale_x = uint32_t(sx);
      r.sample_scale_y = uint32_t(sy);
      r.scale = copy_scaled ? res_scale_ : 1.0f;
    }
    // Destination offset: the source position plus the sub-texture tile (in the source's sample space).
    if (copy_scaled) {
      sub_x = Sc(sub_x);
      sub_y = Sc(sub_y);
    }
    const int32_t dst_x0 = x0 + sub_x * sx, dst_y0 = y0 + sub_y * sy;
    if (r.image.format != resolved_format || dst_x0 + (x1 - x0) > int32_t(r.image.width) ||
        dst_y0 + (y1 - y0) > int32_t(r.image.height)) {
      return true;  // does not fit the containing texture: leave it (logged in the trace)
    }
    if (Tracing()) {
      REXGPU_INFO("[native trace] resolve {} {}@{} fmt {} msaa {} rect {},{}-{},{} -> {:08X} fmt {} {}x{} clear c{} d{} "
                  "endian {} swap {} number {} bias {}",
                  draws_this_frame_, is_depth ? "D" : "C",
                  is_depth ? uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base)
                           : uint32_t(regs.Get<reg::RB_COLOR_INFO>(
                                          reg::RB_COLOR_INFO::rt_register_indices[info.rb_copy_control.copy_src_select])
                                          .color_base),
                  uint32_t(src->format), uint32_t(surface_info.msaa_samples), x0, y0, x1, y1, dest_base,
                  uint32_t(dest_info.copy_dest_format), dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height,
                  uint32_t(info.IsClearingColor()), uint32_t(info.IsClearingDepth()),
                  uint32_t(dest_info.copy_dest_endian), uint32_t(dest_info.copy_dest_swap),
                  uint32_t(dest_info.copy_dest_number), int32_t(dest_info.copy_dest_exp_bias));
    }
    r.frame = frame_;
    if (depth_as_color) {
      const bool float24 =
          xenos::DepthRenderTargetFormat(regs.Get<reg::RB_DEPTH_INFO>().depth_format) ==
          xenos::DepthRenderTargetFormat::kD24FS8;
      if (Convert(kConvertDepthToRgba8, *src, r.image, x0, y0,
                  {{dst_x0, dst_y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, float24)) {
        ++r.generation;
      }
    } else {
    // v106: the same pixels again (source not written since the last copy into this unchanged entry, same
    // rectangle): the copy would not change the texture. DS2 resolves its depth buffer up to 3 times per frame.
    const uint32_t aspect_key = is_depth ? (REXCVAR_GET(ds2_native_resolve_depth_only) ? 1u : 2u) : 0u;
    const int32_t rect_key[6] = {x0, y0, x1, y1, dst_x0, dst_y0};
    if (REXCVAR_GET(ds2_native_resolve_skip_unchanged) && copy_src == src && src->write_id != 0 &&
        r.last_src == src->image && r.last_src_write == src->write_id && r.image.write_id != 0 &&
        r.last_dst_write == r.image.write_id && r.last_generation == r.generation && r.last_aspect == aspect_key &&
        std::memcmp(r.last_rect, rect_key, sizeof(rect_key)) == 0) {
      ++stats_.resolves_skipped;
      if (Tracing()) REXGPU_INFO("[native trace]   resolve skipped: source unchanged since the last copy");
    } else {
    if (profiling_) {
      Mark(fmt::format("resolve copy {} {}x{}", is_depth ? "depth" : "color", src->width, src->height));
    }
    ResolveBarrier(*copy_src, r.image, true);
    VkImageCopy c = {};
    // RC68: textures sample only the depth of a depth resolve; the stencil plane is not copied by default.
    c.srcSubresource = {VkImageAspectFlags(is_depth ? (REXCVAR_GET(ds2_native_resolve_depth_only)
                                                           ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                           : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                    : VK_IMAGE_ASPECT_COLOR_BIT),
                        0, 0, 1};
    c.dstSubresource = c.srcSubresource;
    c.srcOffset = {x0, y0, 0};
    c.dstOffset = {dst_x0, dst_y0, 0};
    c.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0), 1};
    work_since_barrier_ = true;
    cmd_copy_image_(slot().cmd, copy_src->image, VK_IMAGE_LAYOUT_GENERAL, r.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    ResolveBarrier(*copy_src, r.image, false);
    if (is_depth && REXCVAR_GET(ds2_native_resolve_float24) &&
        xenos::DepthRenderTargetFormat(regs.Get<reg::RB_DEPTH_INFO>().depth_format) ==
            xenos::DepthRenderTargetFormat::kD24FS8) {
      QuantizeDepthFloat24(*copy_src, r.image, x0, y0,
                           {{dst_x0, dst_y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}});
    }
    ++r.generation;
    Written(r.image);
    if (copy_src == src) {
      r.last_src = src->image;
      r.last_src_write = src->write_id;
      r.last_dst_write = r.image.write_id;
      r.last_generation = r.generation;
      r.last_aspect = aspect_key;
      std::memcpy(r.last_rect, rect_key, sizeof(rect_key));
    } else {
      r.last_src = VK_NULL_HANDLE;
    }
    // RC70: in a capture, every color resolve is also written as it is at this point of the frame (with alpha),
    // so chains that reuse one address (blur, depth of field, sun shafts) can be followed step by step.
    if (Tracing() && combo_dumps_ && !is_depth && REXCVAR_GET(ds2_native_capture_resolves)) {
      DumpImage(fmt::format("snap_{:04}_{:08X}_g{}", draws_this_frame_, dest_base, r.guest_format), r.image, true);
    }
    }  // v106: copy not skipped
    }
  }

  // Clears (of the whole resolve rectangle, also when the copy was clipped to the destination).
  const VkRect2D rect = {{x0, y0}, {uint32_t(clear_x1 - x0), uint32_t(clear_y1 - y0)}};
  if (info.IsClearingDepth()) {
    const auto di = regs.Get<reg::RB_DEPTH_INFO>();
    NativeImage* d = GetRenderTarget(di.depth_base, sample_pitch, uint32_t(di.depth_format), true, guest_y1);
    if (d) {
      VkClearValue cv = {};
      const uint32_t raw = info.rb_depth_clear;
      if (di.depth_format == xenos::DepthRenderTargetFormat::kD24S8) {
        cv.depthStencil.depth = float(raw >> 8) / float(0xFFFFFF);
      } else {
        cv.depthStencil.depth = xenos::Float20e4To32(raw >> 8);
      }
      cv.depthStencil.stencil = raw & 0xFF;
      ClearRect(*d, cv, rect);
    }
  }
  if (info.IsClearingColor()) {
    const auto ci =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[info.rb_copy_control.copy_src_select]);
    VkClearValue cv = {};
    const uint32_t c32 = info.rb_color_clear;
    switch (ci.color_format) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        for (int i = 0; i < 4; ++i) cv.color.float32[i] = float((c32 >> (8 * i)) & 0xFF) / 255.0f;
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        for (int i = 0; i < 3; ++i) cv.color.float32[i] = float((c32 >> (10 * i)) & 0x3FF) / 1023.0f;
        cv.color.float32[3] = float(c32 >> 30) / 3.0f;
        break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        std::memcpy(&cv.color.float32[0], &c32, 4);
        break;
      default:
        // 64bpp and float formats: zero/one are what games clear to in practice.
        cv.color.float32[0] = cv.color.float32[1] = cv.color.float32[2] = cv.color.float32[3] = c32 ? 1.0f : 0.0f;
        break;
    }
    ClearRect(*src, cv, rect);
  }
  return true;
}

// --- Swap -------------------------------------------------------------------------------------------------------

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  FlushHalfRes();
  ++stats_.swaps;
  {
    const uint64_t now = NowUs();
    const uint64_t frame_us = last_swap_us_ ? now - last_swap_us_ : 0;
    if (last_swap_us_) stats_.us_frame += frame_us;
    last_swap_us_ = now;
    const uint64_t cur[6] = {stats_.us_fence_wait, stats_.us_draws, stats_.us_textures, stats_.us_pipelines,
                             stats_.us_copies, stats_.texture_uploads};
    const int32_t hitch_ms = REXCVAR_GET(ds2_native_hitch_ms);
    {
      // RC80: a frame that waited 40+ ms for the GPU starts (or extends) a spike window of 20 timed frames.
      const uint64_t wait_us = cur[0] - hitch_prev_[0];
      if (spike_frames_) --spike_frames_;
      if (REXCVAR_GET(ds2_native_gpu_spike_ms) > 0 && wait_us >= 40000) {
        if (!spike_frames_ && now - spike_last_log_us_ > 5000000) spike_logs_ = 0;
        if (!spike_frames_) spike_last_log_us_ = now;
        spike_frames_ = 20;
      }
    }
    if (hitch_ms > 0 && frame_us > uint64_t(hitch_ms) * 1000 && draws_this_frame_ > 0) {
      REXGPU_WARN("[native] hitch: frame took {:.1f} ms ({} draws): waiting for the GPU {:.1f}, draws {:.1f} "
                  "(textures {:.1f}, {} uploads; shaders+pipelines {:.1f}), resolves {:.1f}; outside the command "
                  "processor {:.1f} ms | shaders {:.1f} ms, {} pipelines created at once {:.1f} ms, texture texels "
                  "{:.1f} MB",
                  frame_us / 1000.0, draws_this_frame_, (cur[0] - hitch_prev_[0]) / 1000.0,
                  (cur[1] - hitch_prev_[1]) / 1000.0, (cur[2] - hitch_prev_[2]) / 1000.0, cur[5] - hitch_prev_[5],
                  (cur[3] - hitch_prev_[3]) / 1000.0, (cur[4] - hitch_prev_[4]) / 1000.0,
                  (int64_t(frame_us) - int64_t(cur[0] - hitch_prev_[0]) - int64_t(cur[1] - hitch_prev_[1]) -
                   int64_t(cur[4] - hitch_prev_[4])) / 1000.0,
                  (stats_.us_translate - hitch_prev_extra_[0]) / 1000.0, stats_.pipelines_sync - hitch_prev_extra_[1],
                  (stats_.us_pipeline_sync - hitch_prev_extra_[2]) / 1000.0,
                  (stats_.texture_upload_bytes - hitch_prev_extra_[3]) / 1048576.0);
    }
    std::memcpy(hitch_prev_, cur, sizeof(cur));
    hitch_prev_extra_[0] = stats_.us_translate;
    hitch_prev_extra_[1] = stats_.pipelines_sync;
    hitch_prev_extra_[2] = stats_.us_pipeline_sync;
    hitch_prev_extra_[3] = stats_.texture_upload_bytes;
  }
  msaa_heavy_last_frame_.swap(msaa_heavy_this_frame_);
  msaa_heavy_this_frame_.clear();
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  if (!frame_open_ && !BeginFrame()) return;
  EndRenderPass();
  // v106: optional frame cap with even spacing (the presentation of this frame waits for its slot).
#if REX_PLATFORM_SWITCH
  // v116: a 30 lock also holds every frame for two vblanks in the system compositor (swap interval 2), so the
  // frames reach the screen on every other refresh instead of whichever one the timer happened to fall into.
  if (!swap_interval_set_) {
    swap_interval_set_ = true;
    if (REXCVAR_GET(ds2_fps_cap) == 30 && REXCVAR_GET(ds2_fps_lock_vsync) && RexSwitchPerfIntervaloSwap) {
      RexSwitchPerfIntervaloSwap(2);
      REXGPU_INFO("[native] 30 FPS lock on the display refresh (swap interval 2)");
    }
  }
#endif
  if (const int32_t cap = REXCVAR_GET(ds2_fps_cap); cap > 0) {
    const uint64_t period = 1000000u / uint64_t(std::clamp(cap, 10, 60));
    const uint64_t before = NowUs();
    if (cap_next_us_ > before && cap_next_us_ - before <= period) {
      std::this_thread::sleep_for(std::chrono::microseconds(cap_next_us_ - before));
    }
    const uint64_t t = NowUs();
    // Keep the cadence while on time (or late by less than half a frame); restart it after a slow frame.
    cap_next_us_ = (cap_next_us_ && t < cap_next_us_ + period / 2) ? cap_next_us_ + period : t + period;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  // The swap source is texture fetch constant 0 (as in the emulation's RequestSwapTexture).
  const xenos::xe_gpu_texture_fetch_t swap_fetch = register_file_->GetTextureFetch(0);
  const uint32_t base = uint32_t(swap_fetch.base_address) << 12;
  if (Tracing()) {
    REXGPU_INFO("[native trace] swap fetch0 {:08X} fmt {} ptr {:08X} found {}", base, uint32_t(swap_fetch.format),
                frontbuffer_ptr, resolved_.count(base));
  }
  auto rit = resolved_.find(base);
  if (rit == resolved_.end()) rit = resolved_.find(frontbuffer_ptr & 0x1FFFFFFF);
  bool presented_this_swap = false;
  if (presenter && rit != resolved_.end()) {
    NativeImage& src = rit->second.image;
    // v107: a front buffer at the reduced internal resolution covers the same part of its image.
    const float fs = rit->second.scale;
    const uint32_t w = frontbuffer_width ? std::min(uint32_t(std::lround(frontbuffer_width * fs)), src.width) : src.width;
    const uint32_t h =
        frontbuffer_height ? std::min(uint32_t(std::lround(frontbuffer_height * fs)), src.height) : src.height;
    system::X_VIDEO_MODE video_mode;
    kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
    // v116: a reduced internal resolution is upscaled here, with contrast-adaptive sharpening (ds2_sharpen),
    // instead of by the presenter's bilinear filter.
    const bool sharpen = fs < 1.0f && REXCVAR_GET(ds2_sharpen) && frontbuffer_width && frontbuffer_height;
    const uint32_t out_w = sharpen ? frontbuffer_width : w, out_h = sharpen ? frontbuffer_height : h;
    presenter->RefreshGuestOutput(
        out_w, out_h, std::max<uint32_t>(video_mode.display_width, 1), std::max<uint32_t>(video_mode.display_height, 1),
        [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vc = static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          VkCommandBuffer cmd = slot().cmd;
          const VkDevice dev = vulkan_device_->device();
          if (!present_pipeline_) return false;
          const uint64_t fb_key = reinterpret_cast<uint64_t>(vc.image_view()) ^ (vc.image_version() << 48) ^
                                  (uint64_t(out_w) << 16) ^ out_h;
          VkFramebuffer fb;
          auto fit = present_framebuffers_.find(fb_key);
          if (fit != present_framebuffers_.end()) {
            fb = fit->second;
          } else {
            VkImageView view = vc.image_view();
            VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = present_pass_;
            fci.attachmentCount = 1;
            fci.pAttachments = &view;
            fci.width = out_w;
            fci.height = out_h;
            fci.layers = 1;
            if (dfn().vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) return false;
            present_framebuffers_.emplace(fb_key, fb);
          }
          // Gamma ramp (256-entry table, as set by the game) in the upload buffer.
          VkDeviceSize ramp_offset;
          uint8_t* ramp = Upload(256 * 4, std::max<VkDeviceSize>(device()->properties().minUniformBufferOffsetAlignment,
                                                                   16),
                                 ramp_offset);
          if (!ramp) return false;
          std::memcpy(ramp, gamma_ramp_256_entry_table(), 256 * 4);
          VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
          ai.descriptorPool = slot().descriptor_pool;
          ai.descriptorSetCount = 1;
          ai.pSetLayouts = &present_set_layout_;
          VkDescriptorSet set;
          if (dfn().vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return false;
          VkImageView present_view = src.view;
          {
            uint32_t host = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
            if (rit->second.rb_swapped) host = 2 | (1 << 3) | (0 << 6) | (3 << 9);
            VkImageView v = GetSwizzleView(rit->second.swizzle_views, src, swap_fetch.swizzle | (host << 12));
            if (v) present_view = v;
          }
          VkDescriptorImageInfo ii = {convert_sampler_, present_view, VK_IMAGE_LAYOUT_GENERAL};
          VkDescriptorBufferInfo bi = {slot().upload, ramp_offset, 256 * 4};
          VkWriteDescriptorSet wr[2] = {};
          wr[0].sType = wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          wr[0].dstSet = wr[1].dstSet = set;
          wr[0].dstBinding = 0;
          wr[0].descriptorCount = 1;
          wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          wr[0].pImageInfo = &ii;
          wr[1].dstBinding = 1;
          wr[1].descriptorCount = 1;
          wr[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          wr[1].pBufferInfo = &bi;
          dfn().vkUpdateDescriptorSets(dev, 2, wr, 0, nullptr);
          Barrier();
          Mark("present");
          VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
          rbi.renderPass = present_pass_;
          rbi.framebuffer = fb;
          rbi.renderArea = {{0, 0}, {out_w, out_h}};
          work_since_barrier_ = true;
          InvalidateDrawState();
          dfn().vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
          dfn().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
          dfn().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1, &set, 0,
                                        nullptr);
          struct {
            float scale[2];
            int32_t use_ramp;
            float inv_gamma;
            float src_max[2];
            int32_t sharpen;
            float sharpness;
          } pc = {{1.0f, 1.0f}, REXCVAR_GET(ds2_native_gamma_ramp) ? 1 : 0,
                  float(1.0 / std::clamp(REXCVAR_GET(ds2_native_present_gamma), 0.5, 2.5)),
                  {float(w) - 1.0f, float(h) - 1.0f}, 0, float(std::clamp(REXCVAR_GET(ds2_sharpness), 0.0, 1.0))};
          if (sharpen) {
            pc.scale[0] = float(w) / float(out_w);
            pc.scale[1] = float(h) / float(out_h);
            pc.sharpen = 1;
          }
          LogGammaRamp();
          dfn().vkCmdPushConstants(cmd, present_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
          VkViewport vpp = {0.0f, 0.0f, float(out_w), float(out_h), 0.0f, 1.0f};
          VkRect2D sr = {{0, 0}, {out_w, out_h}};
          dfn().vkCmdSetViewport(cmd, 0, 1, &vpp);
          dfn().vkCmdSetScissor(cmd, 0, 1, &sr);
          work_since_barrier_ = true;
          dfn().vkCmdDraw(cmd, 3, 1, 0, 0);
          dfn().vkCmdEndRenderPass(cmd);
          SubmitFrame();
          presented_this_swap = true;
          ++stats_.swaps_presented;
          return true;
        });
  }
  // v54: submit only when the present did not (v51-v53 submitted an extra empty command buffer here every frame,
  // so the 3 frame slots held 1.5 guest frames and the command processor waited for the GPU twice per frame).
  if (frame_open_ && !presented_this_swap) SubmitFrame();
  LogStats();
  EvictTextures();
  // Development dump: at the N-th frame with more than 200 draws (in game, not menus).
  const bool heavy = draws_this_frame_ > 200;
  if (heavy) ++heavy_frames_;
  draws_this_frame_ = 0;
  if (REXCVAR_GET(ds2_native_dump_swap) > 0 && heavy_frames_ == uint64_t(REXCVAR_GET(ds2_native_dump_swap)) &&
      !dumped_ && !DumpDir().empty()) {
    dumped_ = true;
    DumpImages();
  }
  // v55: capture on request (hold both sticks pressed, L3 + R3, for about a second): the next in-game frame is
  // traced into the log and its render targets are written as images.
  if (combo_tracing_ && heavy) {
    combo_tracing_ = false;
    DumpImages();
    REXGPU_WARN("[native] capture {} done (trace above, images in {})", combo_dumps_, DumpDir());
  }
  PollDumpCombo();
}

std::string NativeCommandProcessor::DumpDir() const {
  std::string d = REXCVAR_GET(ds2_native_dump_dir);
#if REX_PLATFORM_SWITCH
  if (d.empty()) d = "sdmc:/switch/ds2/native_dump";
#endif
  if (!d.empty() && combo_dumps_) d += fmt::format("/capture_{:02}", combo_dumps_);
  return d;
}

void NativeCommandProcessor::PollDumpCombo() {
#if REX_PLATFORM_SWITCH
  if (!REXCVAR_GET(ds2_native_capture_combo) || combo_tracing_) return;
  combo_held_frames_ = RexSwitchStickClicksHeld() ? combo_held_frames_ + 1 : 0;
  if (combo_held_frames_ == 20 && combo_dumps_ < 20) {
    ++combo_dumps_;
    const std::string dir = DumpDir();
    mkdir("sdmc:/switch/ds2/native_dump", 0777);
    mkdir(dir.c_str(), 0777);
    combo_tracing_ = true;
    REXGPU_WARN("[native] capture {} requested (L3+R3): tracing the next in-game frame into {}", combo_dumps_, dir);
  }
#endif
}

uint32_t NativeCommandProcessor::ReadPixel(NativeImage& img, uint32_t x, uint32_t y, VkImageAspectFlags aspect) {
  // Development only: one texel through a readback buffer (submits and waits).
  const VkDevice dev = vulkan_device_->device();
  VkBuffer buf;
  VkDeviceMemory mem;
  uint32_t type;
  VkDeviceSize msize;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(vulkan_device_, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         ui::vulkan::util::MemoryPurpose::kReadback, buf, mem, &type,
                                                         &msize)) {
    return 0xDEADBEEF;
  }
  EndRenderPass();
  Barrier();
  VkBufferImageCopy r = {};
  r.imageSubresource = {aspect, 0, 0, 1};
  r.imageOffset = {int32_t(x), int32_t(y), 0};
  r.imageExtent = {1, 1, 1};
  work_since_barrier_ = true;
  dfn().vkCmdCopyImageToBuffer(slot().cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
  FrameSlot& sl = slot();
  SubmitFrame();
  dfn().vkWaitForFences(dev, 1, &sl.fence, VK_TRUE, UINT64_MAX);
  void* map = nullptr;
  uint32_t v = 0;
  dfn().vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map);
  std::memcpy(&v, map, aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : 4);
  dfn().vkUnmapMemory(dev, mem);
  dfn().vkDestroyBuffer(dev, buf, nullptr);
  dfn().vkFreeMemory(dev, mem, nullptr);
  return v;
}

void NativeCommandProcessor::EvictTextures() {
  // Guest textures not used for a while are released (the game streams new ones into new addresses as the player
  // moves). Resolve targets too, if their address has not been written or sampled for long.
  if (frame_ % 60) return;
  const uint64_t keep_frames = uint64_t(std::max<int32_t>(REXCVAR_GET(ds2_native_texture_keep_frames), 30));
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (frame_ - it->second.last_use_frame > keep_frames) {
      for (auto& [sw, v] : it->second.swizzle_views) slot().deferred_views.push_back(v);
      DestroyImage(it->second.image, true);
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = resolved_.begin(); it != resolved_.end();) {
    if (frame_ - it->second.frame > keep_frames * 4) {
      DestroyResolveLogicalImages(it->second, true);
      for (auto& [sw, v] : it->second.swizzle_views) slot().deferred_views.push_back(v);
      DestroyImage(it->second.image, true);
      it = resolved_.erase(it);
    } else {
      ++it;
    }
  }
}

void NativeCommandProcessor::DumpImages() {
  ExportAllMsaaShadows();
  std::vector<std::pair<std::string, NativeImage*>> list;
  for (auto& [k, img] : render_targets_) {
    list.emplace_back(fmt::format("rt_{:04}_{}_f{}_{}x{}", uint32_t(k & 0xFFF), img.depth ? "d" : "c",
                                  uint32_t((k >> 28) & 0xFF), img.width, img.height),
                      &img);
  }
  for (auto& [a, r] : resolved_) {
    list.emplace_back(fmt::format("res_{:08X}_g{}_{}x{}", a, r.guest_format, r.image.width, r.image.height),
                      &r.image);
  }
  for (auto& [name, img] : list) DumpImage(name, *img, false);
  REXGPU_INFO("[native] dumped {} images to {}", list.size(), DumpDir());
}

// Writes one image as <name>.ppm (RGB clamped to [0, 1]); with alpha also <name>.a.pgm for formats with alpha.
// Submits the command buffer and waits for the GPU (development captures only).
void NativeCommandProcessor::DumpImage(const std::string& name, NativeImage& image, bool alpha) {
  NativeImage* img = &image;
  const VkDevice dev = vulkan_device_->device();
  {
    if (!img->image) return;
    const VkDeviceSize bytes = VkDeviceSize(img->width) * img->height * 8;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t type;
    VkDeviceSize msize;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(vulkan_device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                           ui::vulkan::util::MemoryPurpose::kReadback, buf, mem,
                                                           &type, &msize)) {
      return;
    }
    if (!frame_open_) BeginFrame();
    VkBufferImageCopy r = {};
    r.imageSubresource = {VkImageAspectFlags(img->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0,
                          1};
    r.imageExtent = {img->width, img->height, 1};
    Barrier();
    work_since_barrier_ = true;
    dfn().vkCmdCopyImageToBuffer(slot().cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    FrameSlot& sl = slot();
    SubmitFrame();
    dfn().vkWaitForFences(dev, 1, &sl.fence, VK_TRUE, UINT64_MAX);
    void* map = nullptr;
    dfn().vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map);
    std::vector<uint8_t> rgb(size_t(img->width) * img->height * 3);
    std::vector<uint8_t> alpha_bytes;
    const bool has_alpha = alpha && (img->format == VK_FORMAT_R8G8B8A8_UNORM ||
                                     img->format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                     img->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    if (has_alpha) alpha_bytes.resize(size_t(img->width) * img->height);
    const uint8_t* p = static_cast<const uint8_t*>(map);
    auto half = [](uint16_t h) {
      const uint32_t e = (h >> 10) & 31, m = h & 1023;
      float v = e ? std::ldexp(float(m | 1024), int(e) - 25) : std::ldexp(float(m), -24);
      return (h & 0x8000) ? -v : v;
    };
    for (size_t i = 0; i < size_t(img->width) * img->height; ++i) {
      float c[3] = {};
      switch (img->format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
          for (int j = 0; j < 3; ++j) c[j] = p[i * 4 + j] / 255.0f;
          if (has_alpha) alpha_bytes[i] = p[i * 4 + 3];
          break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
          uint32_t v;
          std::memcpy(&v, p + i * 4, 4);
          for (int j = 0; j < 3; ++j) c[j] = float((v >> (10 * j)) & 1023) / 1023.0f;
          if (has_alpha) alpha_bytes[i] = uint8_t((v >> 30) * 85);
          break;
        }
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
          uint16_t v[4];
          std::memcpy(v, p + i * 8, 8);
          for (int j = 0; j < 3; ++j) c[j] = half(v[j]);
          if (has_alpha) alpha_bytes[i] = uint8_t(std::clamp(half(v[3]), 0.0f, 1.0f) * 255.0f + 0.5f);
          break;
        }
        case VK_FORMAT_R16G16_SFLOAT: {
          uint16_t v[2];
          std::memcpy(v, p + i * 4, 4);
          c[0] = half(v[0]);
          c[1] = half(v[1]);
          break;
        }
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
          std::memcpy(&c[0], p + i * 4, 4);
          c[1] = c[2] = c[0];
          break;
        case VK_FORMAT_D24_UNORM_S8_UINT: {
          uint32_t v;
          std::memcpy(&v, p + i * 4, 4);
          c[0] = c[1] = c[2] = float(v & 0xFFFFFF) / float(0xFFFFFF);
          break;
        }
        default: {
          uint32_t v;
          std::memcpy(&v, p + i * 4, 4);
          c[0] = float(v & 0xFF) / 255.0f;
          c[1] = float((v >> 8) & 0xFF) / 255.0f;
          c[2] = float((v >> 16) & 0xFF) / 255.0f;
          break;
        }
      }
      for (int j = 0; j < 3; ++j) rgb[i * 3 + j] = uint8_t(std::clamp(c[j], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    dfn().vkUnmapMemory(dev, mem);
    dfn().vkDestroyBuffer(dev, buf, nullptr);
    dfn().vkFreeMemory(dev, mem, nullptr);
    const std::string path = DumpDir() + "/" + name + ".ppm";
    // On the console the images are halved when wide (SD card time and space).
    uint32_t step = 1;
#if REX_PLATFORM_SWITCH
    if (img->width > 800) step = 2;
#endif
    uint32_t ow = img->width / step, oh = img->height / step;
    // Render targets of unknown height are 2048 tall: keep only the rows that were drawn (non-black).
    uint32_t last_row = 0;
    for (uint32_t y = 0; y < img->height; ++y) {
      const uint8_t* row = rgb.data() + size_t(y) * img->width * 3;
      for (uint32_t x = 0; x < img->width * 3; ++x) {
        if (row[x]) {
          last_row = y;
          break;
        }
      }
    }
    oh = std::max<uint32_t>(1, std::min(oh, (last_row + step) / step));
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", ow, oh);
      if (step == 1) {
        std::fwrite(rgb.data(), 1, size_t(ow) * oh * 3, f);
      } else {
        std::vector<uint8_t> small(size_t(ow) * oh * 3);
        for (uint32_t y = 0; y < oh; ++y) {
          for (uint32_t x = 0; x < ow; ++x) {
            std::memcpy(&small[(size_t(y) * ow + x) * 3], &rgb[(size_t(y * step) * img->width + x * step) * 3], 3);
          }
        }
        std::fwrite(small.data(), 1, small.size(), f);
      }
      std::fclose(f);
    }
    if (has_alpha) {
      if (FILE* f = std::fopen((DumpDir() + "/" + name + ".a.pgm").c_str(), "wb")) {
        std::fprintf(f, "P5\n%u %u\n255\n", ow, oh);
        for (uint32_t y = 0; y < oh; ++y) {
          for (uint32_t x = 0; x < ow; ++x) std::fputc(alpha_bytes[size_t(y * step) * img->width + x * step], f);
        }
        std::fclose(f);
      }
    }
  }
}

void NativeCommandProcessor::LogStats() {
  const int32_t every = REXCVAR_GET(ds2_native_log_every);
  if (every <= 0 || stats_.swaps - last_log_frame_ < uint64_t(every)) return;
  const double n = double(stats_.swaps - last_log_frame_);
  last_log_frame_ = stats_.swaps;
  if (REXCVAR_GET(ds2_native_probe)) {
    REXGPU_INFO("[native] probe interval {}: mode {} {} (1 = no pixels, 2 = one triangle per draw)", probe_intervals_,
                REXCVAR_GET(ds2_native_probe), probe_active_ ? "ON, picture wrong on purpose" : "OFF, normal frames");
  }
  REXGPU_INFO(
      "[native] per frame: {:.0f} draws ({:.1f} skipped, {:.1f} unsupported prim, {:.1f} failed), {:.1f} resolves, "
      "({:.1f} skipped as unchanged), {:.1f} clears, {:.1f} passes, {:.1f} conversions, {:.1f} texture uploads ({:.1f} from resolves, {:.1f} unsupported), {:.2f} MB "
      "uploaded | totals: {} pipelines, {} render targets, {} textures, {} resolve targets, presented {}/{} swaps",
      stats_.draws / n, stats_.draws_skipped / n, stats_.draws_unsupported_prim / n, stats_.draws_failed / n,
      stats_.copies / n, stats_.resolves_skipped / n, stats_.clears / n, stats_.render_passes_begun / n, stats_.conversions / n, stats_.texture_uploads / n,
      stats_.texture_from_resolve / n, stats_.texture_unsupported / n, stats_.upload_bytes / n / 1048576.0,
      pipelines_.size(), render_targets_.size(), textures_.size(), resolved_.size(), stats_.swaps_presented,
      stats_.swaps);
  REXGPU_INFO("[native] barriers/frame: {:.1f} issued, {:.1f} elided | draw state groups/frame: {:.1f} recorded, "
              "{:.1f} skipped as redundant", stats_.barriers_issued / n, stats_.barriers_elided / n,
              stats_.state_calls / n, stats_.state_calls_skipped / n);
  REXGPU_INFO("[native] host 4x MSAA/frame: {:.1f} draws, {:.2f} imports ({:.2f} skipped after a clear), {:.2f} exports"
              " | shadow face clears/frame {:.1f}",
              stats_.msaa_draws / n, stats_.msaa_imports / n, stats_.msaa_imports_skipped / n,
              stats_.msaa_exports / n, stats_.face_clears / n);
  REXGPU_INFO("[native] reduced-resolution particle draws/frame: {:.2f} (1/{} resolution), composites/frame {:.2f} | "
              "textures rewritten by the game/frame {:.2f} | reduced depth buffers/frame {:.2f}",
              stats_.halfres_draws / n, halfres_divisor_, stats_.halfres_composites / n,
              stats_.dynamic_texture_updates / n, stats_.halfres_depth_downsamples / n);
  REXGPU_INFO("[native] texture fetches/frame: {:.1f} 1D, {:.1f} cube ({:.2f} faces copied from resolves), {:.1f} 3D, "
              "{:.1f} white (unsupported 3D/cube/other dimension), {:.1f} depth from 8888 ({:.2f} decodes), "
              "{:.2f} float24 depth resolves",
              stats_.texture_1d / n, stats_.texture_cube / n, stats_.cube_faces_from_resolves / n, stats_.texture_3d / n,
              stats_.texture_white_dimension / n, stats_.depth_decoded_fetches / n, stats_.depth_decodes / n,
              stats_.depth_float24_quantized / n);
  std::string prims;
  for (uint32_t i = 0; i < 32; ++i) {
    if (stats_.prim_counts[i]) prims += fmt::format(" {}:{:.0f}", i, stats_.prim_counts[i] / n);
  }
  REXGPU_INFO("[native] early-z draws per frame: {:.0f} | pipelines: {} new on the worker ({:.1f} ms of compile), "
              "{:.1f} draws per frame skipped waiting for one, {} needed at once taken from the queue",
              stats_.early_z_draws / n, stats_.pipelines_async, pipeline_async_us_.exchange(0) / 1000.0,
              stats_.draws_pipeline_pending / n, stats_.pipelines_stolen);
  REXGPU_INFO("[native] primitive types per frame:{} | skipped: no vertex shader {:.1f}, no rasterization {:.1f}, "
              "no pitch {:.1f}, no target {:.1f}",
              prims, stats_.skip_no_vs / n, stats_.skip_no_raster / n, stats_.skip_no_pitch / n,
              stats_.skip_no_target / n);
  // v54: where the time goes. CPU = command processor thread (draws include textures and pipelines).
  REXGPU_INFO("[native] cpu ms per frame: frame {:.2f}, waiting for the GPU {:.2f}, draws {:.2f} (textures {:.2f}, "
              "shaders+pipelines {:.2f}), resolves {:.2f} | {:.2f} texture levels, {:.2f} MB texture data, "
              "{:.2f} submits per frame | occlusion queries: {:.1f} begun, {:.1f} ended, {:.1f} visible, {:.2f} lost",
              stats_.us_frame / n / 1000.0, stats_.us_fence_wait / n / 1000.0, stats_.us_draws / n / 1000.0,
              stats_.us_textures / n / 1000.0, stats_.us_pipelines / n / 1000.0, stats_.us_copies / n / 1000.0,
              stats_.texture_levels / n, stats_.texture_upload_bytes / n / 1048576.0, stats_.submits / n,
              stats_.occlusion_begins / n, stats_.occlusion_ends / n, stats_.occlusion_visible / n,
              stats_.occlusion_lost / n);
  REXGPU_INFO("[native] draw detail ms/frame: geometry {:.2f}, constants {:.2f}, descriptors {:.2f}, commands {:.2f} | "
              "texture descriptor sets/frame: {:.1f} reused ({:.1f} kept across frames), {:.1f} created, "
              "{:.2f} dropped | {} kept",
              stats_.us_geometry / n / 1000.0, stats_.us_constants / n / 1000.0,
              stats_.us_descriptors / n / 1000.0, stats_.us_commands / n / 1000.0,
              stats_.texture_sets_reused / n, stats_.texture_sets_kept / n, stats_.texture_sets_created / n,
              stats_.texture_sets_dropped / n, persistent_texture_sets_.size());
  REXGPU_INFO("[native] constant sets/frame: {:.1f} reused, {:.1f} created | image resolve barriers/frame {:.1f}",
              stats_.constant_sets_reused / n, stats_.constant_sets_created / n, stats_.resolve_image_barriers / n);
  REXGPU_INFO("[native] draw setup ms/frame {:.2f} (before geometry, without textures and shaders) | constant "
              "uploads reused/frame {:.1f} | in the interval: shaders translated or restored {:.1f} ms, {} pipelines "
              "created at once {:.1f} ms",
              stats_.us_prelude / n / 1000.0, stats_.constant_uploads_reused / n, stats_.us_translate / 1000.0,
              stats_.pipelines_sync, stats_.us_pipeline_sync / 1000.0);
  {
    const uint64_t rc = restore_count_.exchange(0), rus = restore_us_.exchange(0);
    REXGPU_INFO("[native] shader pack restores on the worker in the interval: {} queued, {} done, {:.1f} ms ({:.2f} "
                "ms each), {:.1f} draws per frame skipped waiting for one",
                stats_.shader_restores_async, rc, rus / 1000.0, rc ? rus / 1000.0 / rc : 0.0,
                stats_.draws_shader_pending / n);
  }
  REXGPU_INFO("[native] immutable uploads/frame: {:.1f} hits, {:.2f} MB saved",
              stats_.content_hits / n, stats_.content_saved_bytes / n / 1048576.0);
  REXGPU_INFO("[native] vertex draws/frame: {:.1f} hardware inputs, {:.1f} legacy fetch",
              stats_.native_vertex_draws / n, stats_.legacy_vertex_draws / n);
  if (profile_capable_ && profile_buffers_) {
    // RC66: only 1 command buffer in ds2_native_gpu_profile_every is timed; scale to all buffers.
    const double sampled_scale = double(stats_.submits) / double(profile_buffers_);
    std::vector<ProfileLabel*> order;
    double sum = 0;
    for (ProfileLabel& l : profile_labels_) {
      if (l.count) order.push_back(&l);
      sum += l.ms;
    }
    std::sort(order.begin(), order.end(), [](const ProfileLabel* a, const ProfileLabel* b) { return a->ms > b->ms; });
    std::string top;
    for (size_t i = 0; i < order.size() && i < 16; ++i) {
      top += fmt::format("\n[native gpu]   {:6.2f} ms {:5.1f}%  x{:.1f}  {:.0f} draws  {}",
                         order[i]->ms * sampled_scale / n, sum > 0 ? 100.0 * order[i]->ms / sum : 0.0,
                         order[i]->count * sampled_scale / n, order[i]->draws * sampled_scale / n, order[i]->name);
    }
    REXGPU_INFO("[native gpu] GPU ms per frame: {:.2f} (command buffers {:.2f} per frame), by pass kind:{}",
                profile_gpu_ms_ * sampled_scale / n, stats_.submits / n, top);
    std::string shader_top;
    uint32_t shader_groups_logged = 0;
    for (const auto* label : order) {
      if (label->name.rfind("shader VS ", 0) != 0) continue;
      shader_top += fmt::format("\n[native shader gpu] {:.3f} ms/group, {} groups, {:.1f} draws/group: {}",
                                label->ms / double(label->count), label->count,
                                double(label->draws) / double(label->count), label->name);
      if (++shader_groups_logged == 16) break;
    }
    if (!shader_top.empty()) REXGPU_INFO("[native shader gpu] sampled groups (not a frame average):{}", shader_top);
    for (ProfileLabel& l : profile_labels_) l = {l.name};
    profile_gpu_ms_ = 0;
    profile_buffers_ = 0;
  }
  const uint64_t pipelines = stats_.pipelines, swaps = stats_.swaps, presented = stats_.swaps_presented;
  stats_ = Stats();
  std::memset(hitch_prev_, 0, sizeof(hitch_prev_));
  std::memset(hitch_prev_extra_, 0, sizeof(hitch_prev_extra_));
  stats_.pipelines = pipelines;
  stats_.swaps = swaps;
  stats_.swaps_presented = presented;
  // RC90: two intervals off, two on; switching right after the report keeps every interval pure.
  ++probe_intervals_;
  probe_active_ = ((probe_intervals_ / 2) & 1) ? uint32_t(REXCVAR_GET(ds2_native_probe)) : 0u;
}

// --- Graphics system ---------------------------------------------------------------------------------------------

class NativeGraphicsSystem final : public GraphicsSystem {
 public:
  NativeGraphicsSystem() = default;
  std::string name() const override { return "DS2 native (Vulkan)"; }

 protected:
  void CreateProvider(bool with_presentation) override {
    provider_ = ui::vulkan::VulkanProvider::Create(true, with_presentation);
  }
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace

rex::system::IGraphicsSystem* CreateNativeGraphicsSystem() {
  REXGPU_INFO("[native] DS2 native renderer selected (ds2_native_renderer = true); Xenos emulation not used");
  return new NativeGraphicsSystem();
}

}  // namespace rex::graphics::native
