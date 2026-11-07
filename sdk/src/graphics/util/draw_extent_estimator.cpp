/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cfloat>
#include <cstdint>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/ui/graphics_util.h>

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu, false, "GPU",
                    "Execute unclipped draw vertex shader on CPU");

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu_with_scissor, false, "GPU",
                    "Execute unclipped draw VS on CPU with scissor");

// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu, true,
//     "Execute the vertex shader for draws with clipping disabled, primarily "
//     "screen-space draws (such as clears), on the CPU when possible to estimate "
//     "the extent of the EDRAM involved in the draw.\n"
//     "Enabling this may significantly improve GPU performance as otherwise up "
//     "to the entire EDRAM may be considered used in draws without clipping, "
//     "potentially resulting in spurious EDRAM range ownership transfer round "
//     "trips between host render targets.\n"
//     "Also, on hosts where certain render target formats have to be emulated in "
//     "a lossy way (for instance, 16-bit fixed-point via 16-bit floating-point), "
//     "this prevents corruption of other render targets located after the "
//     "current ones in the EDRAM by lossy range ownership transfers done for "
//     "those draws.",
//     "GPU");
// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu_with_scissor, false,
//     "Don't restrict the usage of execute_unclipped_draw_vs_on_cpu to only "
//     "non-scissored draws (with the right and the bottom sides of the scissor "
//     "rectangle at 8192 or beyond) even though if the scissor rectangle is "
//     "present, it's usually sufficient for esimating the height of the render "
//     "target.\n"
//     "Enabling this may cause excessive processing of vertices on the CPU, as "
//     "some games draw rectangles (for their UI, for instance) without clipping, "
//     "but with a proper scissor rectangle.",
//     "GPU");

namespace rex::graphics {

void DrawExtentEstimator::PositionYExportSink::Export(ucode::ExportRegister export_register,
                                                      const float* value, uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    if (value_mask & 0b0010) {
      position_y_ = value[1];
    }
    if (value_mask & 0b1000) {
      position_w_ = value[3];
    }
  } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if (value_mask & 0b0001) {
      point_size_ = value[0];
    }
    if (value_mask & 0b0100) {
      vertex_kill_ = rex::memory::Reinterpret<uint32_t>(value[2]);
    }
  }
}

uint32_t DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (!vgt_draw_initiator.num_indices) {
    return 0;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    // TODO(Triang3l): Support immediate indices.
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  // Not reproducing tessellation.
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  assert_true(vertex_shader.type() == xenos::ShaderType::kVertex);
  assert_true(vertex_shader.is_ucode_analyzed());
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  union {
    const void* index_buffer;
    const uint16_t* index_buffer_16;
    const uint32_t* index_buffer_32;
  };
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    xenos::IndexFormat index_format = vgt_draw_initiator.index_size;
    uint32_t index_buffer_base = regs[XE_GPU_REG_VGT_DMA_BASE];
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      // Handle the index endianness to same way as the PrimitiveProcessor.
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(sizeof(uint16_t) - 1);
    } else {
      assert_true(vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32);
      index_buffer_base &= ~uint32_t(sizeof(uint32_t) - 1);
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  float viewport_y_scale =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float viewport_y_offset =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;

  int32_t point_vertex_min_diameter_float = 0;
  int32_t point_vertex_max_diameter_float = 0;
  float point_constant_radius_y = 0.0f;
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    point_vertex_min_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.min_size) * (2.0f / 16.0f));
    point_vertex_max_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.max_size) * (2.0f / 16.0f));
    point_constant_radius_y = float(regs.Get<reg::PA_SU_POINT_SIZE>().height) * (1.0f / 16.0f);
  }

  float max_y = -FLT_MAX;

  shader_interpreter_.SetShader(vertex_shader);

  PositionYExportSink position_y_export_sink;
  shader_interpreter_.SetExportSink(&position_y_export_sink);
  for (uint32_t i = 0; i < vgt_draw_initiator.num_indices; ++i) {
    uint32_t vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (i < vgt_dma_size.num_words) {
        if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
          vertex_index = index_buffer_16[i];
        } else {
          vertex_index = index_buffer_32[i];
        }
        // The Xenos only uses 24 bits of the index (reset_indx is 24-bit).
        vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      } else {
        vertex_index = 0;
      }
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        continue;
      }
    } else {
      assert_true(vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex);
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));

    position_y_export_sink.Reset();

    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();

    if (position_y_export_sink.vertex_kill().has_value() &&
        (position_y_export_sink.vertex_kill().value() & ~(UINT32_C(1) << 31))) {
      continue;
    }
    if (!position_y_export_sink.position_y().has_value()) {
      continue;
    }
    float vertex_y = position_y_export_sink.position_y().value();
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!position_y_export_sink.position_w().has_value()) {
        continue;
      }
      vertex_y /= position_y_export_sink.position_w().value();
    }

    vertex_y = vertex_y * viewport_y_scale + viewport_y_offset;

    if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
      float point_radius_y;
      if (position_y_export_sink.point_size().has_value()) {
        // Vertex-specified diameter. Clamped effectively as a signed integer in
        // the hardware, -NaN, -Infinity ... -0 to the minimum, +Infinity, +NaN
        // to the maximum.
        point_radius_y = 0.5f * rex::memory::Reinterpret<float>(std::min(
                                    point_vertex_max_diameter_float,
                                    std::max(point_vertex_min_diameter_float,
                                             rex::memory::Reinterpret<int32_t>(
                                                 position_y_export_sink.point_size().value()))));
      } else {
        // Constant radius.
        point_radius_y = point_constant_radius_y;
      }
      vertex_y += point_radius_y;
    }

    // std::max is `a < b ? b : a`, thus in case of NaN, the first argument is
    // always returned - max_y, which is initialized to a normalized value.
    max_y = std::max(max_y, vertex_y);
  }
  shader_interpreter_.SetExportSink(nullptr);

  int32_t max_y_24p8 = ui::FloatToD3D11Fixed16p8(max_y);
  // 16p8 range is -32768 to 32767+255/256, but it's stored as uint32_t here,
  // as 24p8, so overflowing up to -8388608 to 8388608+255/256 is safe. The
  // range of the window offset plus the half-pixel offset is -16384 to 16384.5,
  // so it's safe to add both - adding it will neither move the 16p8 clamping
  // bounds -32768 and 32767+255/256 into the 0...8192 screen space range, nor
  // cause 24p8 overflow.
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    max_y_24p8 += 128;
  }
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    max_y_24p8 += regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset * 256;
  }
  // Top-left rule - .5 exclusive without MSAA, 1. exclusive with MSAA.
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  return (uint32_t(std::max(int32_t(0), max_y_24p8)) +
          ((rb_surface_info.msaa_samples == xenos::MsaaSamples::k1X) ? 127 : 255)) >>
         8;
}

uint32_t DrawExtentEstimator::EstimateMaxY(bool try_to_estimate_vertex_max_y,
                                           const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  int32_t window_y_offset = pa_sc_window_offset.window_y_offset;

  // Scissor.
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t scissor_bottom = int32_t(pa_sc_window_scissor_br.br_y);
  bool scissor_window_offset = !regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable;
  if (scissor_window_offset) {
    scissor_bottom += window_y_offset;
  }
  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  scissor_bottom = std::min(scissor_bottom, int32_t(pa_sc_screen_scissor_br.br_y));
  uint32_t max_y = uint32_t(std::max(scissor_bottom, int32_t(0)));

  if (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    // Actual extent from the vertices.
    if (try_to_estimate_vertex_max_y && REXCVAR_GET(execute_unclipped_draw_vs_on_cpu)) {
      bool estimate_vertex_max_y;
      if (REXCVAR_GET(execute_unclipped_draw_vs_on_cpu_with_scissor)) {
        estimate_vertex_max_y = true;
      } else {
        estimate_vertex_max_y = false;
        if (scissor_bottom >= xenos::kTexture2DCubeMaxWidthHeight) {
          // Handle just the usual special 8192x8192 case in Direct3D 9 - 8192
          // may be a normal render target height (80x8192 is well within the
          // EDRAM size, for instance), no need to process the vertices on the
          // CPU in this case.
          int32_t scissor_right = int32_t(pa_sc_window_scissor_br.br_x);
          if (scissor_window_offset) {
            scissor_right += pa_sc_window_offset.window_x_offset;
          }
          scissor_right = std::min(scissor_right, int32_t(pa_sc_screen_scissor_br.br_x));
          if (scissor_right >= xenos::kTexture2DCubeMaxWidthHeight) {
            estimate_vertex_max_y = true;
          }
        }
      }
      if (estimate_vertex_max_y) {
        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader));
      }
    }
  } else {
    // Viewport. Though the Xenos itself doesn't have an implicit viewport
    // scissor (it's set by Direct3D 9 when a viewport is used), on hosts, it
    // usually exists and can't be disabled.
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    float viewport_bottom = 0.0f;
    // First calculate all the integer.0 or integer.5 offsetting exactly at full
    // precision.
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      viewport_bottom += float(window_y_offset);
    }
    if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
      viewport_bottom += 0.5f;
    }
    // Then apply the floating-point viewport offset.
    if (pa_cl_vte_cntl.vport_y_offset_ena) {
      viewport_bottom += regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    }
    viewport_bottom += pa_cl_vte_cntl.vport_y_scale_ena
                           ? std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE))
                           : 1.0f;
    // Using floor, or, rather, truncation (because maxing with zero anyway)
    // similar to how viewport scissoring behaves on real AMD, Intel and Nvidia
    // GPUs on Direct3D 12 (but not WARP), also like in
    // draw_util::GetHostViewportInfo.
    // max(0.0f, viewport_bottom) to drop NaN and < 0 - max picks the first
    // argument in the !(a < b) case (always for NaN), min as float (max_y is
    // well below 2^24) to safely drop very large values.
    max_y = uint32_t(std::min(float(max_y), std::max(0.0f, viewport_bottom)));
  }

  return max_y;
}

bool DrawExtentEstimator::EstimateFullyCoveredRectangle(const Shader& vertex_shader, int32_t& x0_out,
                                                        int32_t& y0_out, int32_t& x1_out, int32_t& y1_out,
                                                        bool* inside_clip_volume_out, float* box_out) {
  const RegisterFile& regs = register_file_;
  if (inside_clip_volume_out) {
    *inside_clip_volume_out = false;
  }
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  const xenos::PrimitiveType prim = vgt_draw_initiator.prim_type;
  const uint32_t count = vgt_draw_initiator.num_indices;
  const bool supported = (prim == xenos::PrimitiveType::kRectangleList && count == 3) ||
                         (prim == xenos::PrimitiveType::kQuadList && count == 4) ||
                         (prim == xenos::PrimitiveType::kTriangleStrip && count == 4) ||
                         (prim == xenos::PrimitiveType::kTriangleList && count == 6);
  if (!supported) {
    return false;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return false;
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select == xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  if (!vertex_shader.is_ucode_analyzed() || !ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return false;
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  // Culling could drop one of the triangles.
  if (pa_su_sc_mode_cntl.cull_front || pa_su_sc_mode_cntl.cull_back) {
    return false;
  }

  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  const uint16_t* index_buffer_16 = nullptr;
  const uint32_t* index_buffer_32 = nullptr;
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    if (vgt_dma_size.num_words < count) {
      return false;
    }
    uint32_t index_buffer_base = regs[XE_GPU_REG_VGT_DMA_BASE];
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_16 = reinterpret_cast<const uint16_t*>(
          memory_.TranslatePhysical(index_buffer_base & ~uint32_t(sizeof(uint16_t) - 1)));
    } else {
      index_buffer_32 = reinterpret_cast<const uint32_t*>(
          memory_.TranslatePhysical(index_buffer_base & ~uint32_t(sizeof(uint32_t) - 1)));
    }
  }
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  const float scale_x =
      pa_cl_vte_cntl.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  const float offset_x =
      pa_cl_vte_cntl.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  const float scale_y =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  const float offset_y =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  float half_pixel = regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  const float window_x = pa_su_sc_mode_cntl.vtx_window_offset_enable ? float(pa_sc_window_offset.window_x_offset) : 0.0f;
  const float window_y = pa_su_sc_mode_cntl.vtx_window_offset_enable ? float(pa_sc_window_offset.window_y_offset) : 0.0f;

  float vx[6], vy[6];
  // Clip-space containment: only meaningful when the shader outputs homogeneous x, y, z, w (no VTE pass-through).
  const bool dx_clip_space = regs.Get<reg::PA_CL_CLIP_CNTL>().dx_clip_space_def;
  bool inside_clip_volume = true;
  shader_interpreter_.SetShader(vertex_shader);
  PositionXYExportSink sink;
  shader_interpreter_.SetExportSink(&sink);
  bool ok = true;
  for (uint32_t i = 0; i < count && ok; ++i) {
    uint32_t vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      vertex_index = index_buffer_16 ? uint32_t(index_buffer_16[i]) : index_buffer_32[i];
      vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena &&
          vertex_index == regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx) {
        ok = false;
        break;
      }
    } else {
      vertex_index = i;
    }
    vertex_index = std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));
    sink.Reset();
    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();
    if (sink.killed_ || !sink.x_.has_value() || !sink.y_.has_value()) {
      ok = false;
      break;
    }
    float x = sink.x_.value(), y = sink.y_.value();
    if (i < 4 && clip_out_) {
      clip_out_[4 * i + 0] = x;
      clip_out_[4 * i + 1] = y;
      clip_out_[4 * i + 2] = sink.z_.value_or(-999.0f);
      clip_out_[4 * i + 3] = sink.w_.value_or(-999.0f);
    }
    // The clip-space W, as the translated vertex shader rebuilds it (SpirvShaderTranslator::
    // CompleteVertexOrTessEvalShaderInMain): VTX_W0_FMT set means the shader exports W itself, clear means 1/W.
    float clip_w = std::numeric_limits<float>::quiet_NaN();
    if (sink.w_.has_value()) {
      clip_w = pa_cl_vte_cntl.vtx_w0_fmt ? sink.w_.value() : 1.0f / sink.w_.value();
    }
    if (inside_clip_volume) {
      float z = sink.z_.value_or(std::numeric_limits<float>::quiet_NaN());
      if (pa_cl_vte_cntl.vtx_z_fmt) {
        z *= clip_w;  // exported already divided by W
      }
      // X and Y may be outside: with clipping, the host viewport is the integer rectangle of the guest one
      // (draw_util::GetHostViewportInfo), and the rectangle is intersected with it below. Z must be inside, or
      // near / far clipping could cut the primitive.
      inside_clip_volume = clip_w > 0.0f &&
                           (dx_clip_space ? (z >= 0.0f && z <= clip_w) : (z >= -clip_w && z <= clip_w));
    }
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!(clip_w > 0.0f)) {
        ok = false;
        break;
      }
      x /= clip_w;
      y /= clip_w;
    }
    vx[i] = x * scale_x + offset_x + half_pixel + window_x;
    vy[i] = y * scale_y + offset_y + half_pixel + window_y;
    if (!std::isfinite(vx[i]) || !std::isfinite(vy[i])) {
      ok = false;
    }
  }
  shader_interpreter_.SetExportSink(nullptr);
  if (!ok) {
    return false;
  }

  float min_x = vx[0], max_x = vx[0], min_y = vy[0], max_y = vy[0];
  for (uint32_t i = 1; i < count; ++i) {
    min_x = std::min(min_x, vx[i]);
    max_x = std::max(max_x, vx[i]);
    min_y = std::min(min_y, vy[i]);
    max_y = std::max(max_y, vy[i]);
  }
  if (box_out) {
    // Development: the window-space bounding box and the vertices (up to 4).
    box_out[0] = min_x;
    box_out[1] = min_y;
    box_out[2] = max_x;
    box_out[3] = max_y;
    for (uint32_t i = 0; i < 4; ++i) {
      box_out[4 + 2 * i] = i < count ? vx[i] : -1.0f;
      box_out[5 + 2 * i] = i < count ? vy[i] : -1.0f;
    }
  }
  if (!(max_x > min_x) || !(max_y > min_y)) {
    return false;
  }
  // Every vertex must be a corner of the bounding box.
  for (uint32_t i = 0; i < count; ++i) {
    if ((vx[i] != min_x && vx[i] != max_x) || (vy[i] != min_y && vy[i] != max_y)) {
      return false;
    }
  }
  const double box_area = double(max_x - min_x) * double(max_y - min_y);
  auto tri_area = [&](uint32_t a, uint32_t b, uint32_t c) {
    return 0.5 * std::abs(double(vx[b] - vx[a]) * double(vy[c] - vy[a]) -
                          double(vx[c] - vx[a]) * double(vy[b] - vy[a]));
  };
  if (prim == xenos::PrimitiveType::kRectangleList) {
    // Three corners of the box: the fourth is implied, the rectangle is the box (its area is twice the triangle's).
    if (std::abs(2.0 * tri_area(0, 1, 2) - box_area) > box_area * 1e-6) return false;
  } else {
    double area = 0.0;
    if (prim == xenos::PrimitiveType::kTriangleList) {
      area = tri_area(0, 1, 2) + tri_area(3, 4, 5);
    } else if (prim == xenos::PrimitiveType::kTriangleStrip) {
      area = tri_area(0, 1, 2) + tri_area(1, 2, 3);
    } else {  // quad list: the quad 0-1-2-3 as two triangles
      area = tri_area(0, 1, 2) + tri_area(0, 2, 3);
    }
    if (std::abs(area - box_area) > box_area * 1e-6) return false;
  }

  // Whole pixels inside the box (all sample positions covered), then the scissor.
  int32_t x0 = int32_t(std::ceil(std::max(min_x, -16384.0f)));
  int32_t y0 = int32_t(std::ceil(std::max(min_y, -16384.0f)));
  int32_t x1 = int32_t(std::floor(std::min(max_x, 16384.0f)));
  int32_t y1 = int32_t(std::floor(std::min(max_y, 16384.0f)));
  auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t sx0 = int32_t(tl.tl_x), sy0 = int32_t(tl.tl_y), sx1 = int32_t(br.br_x), sy1 = int32_t(br.br_y);
  if (!tl.window_offset_disable) {
    sx0 += pa_sc_window_offset.window_x_offset;
    sx1 += pa_sc_window_offset.window_x_offset;
    sy0 += pa_sc_window_offset.window_y_offset;
    sy1 += pa_sc_window_offset.window_y_offset;
  }
  auto stl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  x0 = std::max({x0, sx0, int32_t(stl.tl_x), 0});
  y0 = std::max({y0, sy0, int32_t(stl.tl_y), 0});
  x1 = std::min({x1, sx1, int32_t(sbr.br_x)});
  y1 = std::min({y1, sy1, int32_t(sbr.br_y)});
  if (inside_clip_volume_out && !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    // The host clips (or viewport-scissors) to the guest viewport truncated to whole pixels, with the window and
    // half-pixel offsets, as draw_util::GetHostViewportInfo computes it.
    const float vp_offset_x = offset_x + half_pixel + window_x;
    const float vp_offset_y = offset_y + half_pixel + window_y;
    x0 = std::max(x0, int32_t(std::max(vp_offset_x - std::abs(scale_x), 0.0f)));
    y0 = std::max(y0, int32_t(std::max(vp_offset_y - std::abs(scale_y), 0.0f)));
    x1 = std::min(x1, int32_t(std::max(vp_offset_x + std::abs(scale_x), 0.0f)));
    y1 = std::min(y1, int32_t(std::max(vp_offset_y + std::abs(scale_y), 0.0f)));
  }
  if (x1 <= x0 || y1 <= y0) {
    return false;
  }
  x0_out = x0;
  y0_out = y0;
  x1_out = x1;
  y1_out = y1;
  if (inside_clip_volume_out) {
    *inside_clip_volume_out = inside_clip_volume;
  }
  return true;
}

}  // namespace rex::graphics
