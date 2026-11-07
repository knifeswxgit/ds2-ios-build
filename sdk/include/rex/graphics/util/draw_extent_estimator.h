#pragma once
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

#include <cstdint>
#include <optional>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const memory::Memory& memory)
      : register_file_(register_file),
        memory_(memory),
        shader_interpreter_(register_file, memory) {}

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y, const Shader& vertex_shader);

  // DS2 port: if the draw is provably one axis-aligned screen-space rectangle (a rectangle list of 3 vertices, a
  // quad list or triangle strip of 4, or a triangle list of 6, whose vertices are all corners of their bounding box
  // and whose triangles add up exactly to it), the pixels whose whole area it covers, in window coordinates and
  // clipped to the scissor: [x0, x1) x [y0, y1). Every sample of those pixels is covered. False otherwise.
  // inside_clip_volume_out (optional): whether every vertex is inside the clip volume (|x|, |y| <= w, z in the
  // clip range), so that the clipper, when enabled, cannot cut anything off.
  bool EstimateFullyCoveredRectangle(const Shader& vertex_shader, int32_t& x0, int32_t& y0, int32_t& x1,
                                     int32_t& y1, bool* inside_clip_volume_out = nullptr,
                                     float* box_out = nullptr);

 public:
  // Development: when set, EstimateFullyCoveredRectangle stores the clip-space x, y, z, w of up to 4 vertices.
  float* clip_out_ = nullptr;

 private:
  class PositionXYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value, uint32_t value_mask) override {
      if (export_register == ucode::ExportRegister::kVSPosition) {
        if (value_mask & 0b0001) x_ = value[0];
        if (value_mask & 0b0010) y_ = value[1];
        if (value_mask & 0b0100) z_ = value[2];
        if (value_mask & 0b1000) w_ = value[3];
      } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex &&
                 (value_mask & 0b0100)) {
        killed_ = true;
      }
    }
    void Reset() {
      x_.reset();
      y_.reset();
      z_.reset();
      w_.reset();
      killed_ = false;
    }
    std::optional<float> x_, y_, z_, w_;
    bool killed_ = false;
  };

  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const memory::Memory& memory_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace rex::graphics
