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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/provider.h>

namespace rex::graphics::vulkan {

// v107: reduced internal resolution (DS2 native renderer, ds2_render_scale). When set, every pixel shader gets
// gl_FragCoord.xy multiplied by a specialization constant (ID kFragCoordScaleSpecId, default 1.0), so a pipeline
// for a scaled render target can make the shader see guest pixel positions. Set before the first draw.
std::atomic<bool> g_fragcoord_scale_patch{false};
constexpr uint32_t kFragCoordScaleSpecId = 0x5400;

namespace {

// Returns false (module unchanged) when the shader has no gl_FragCoord or reads it in a way not handled here.
// Handled: loads of the whole vec4, and loads through OpAccessChain with a constant index (x and y scaled).
bool PatchFragCoordScale(std::vector<uint32_t>& w) {
  if (w.size() < 5 || w[0] != 0x07230203u) return false;
  const size_t n = w.size();
  uint32_t var = 0;
  size_t decorate_end = 0;
  for (size_t i = 5; i < n;) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (!wc || i + wc > n) return false;
    if (op == 71 && wc >= 4 && w[i + 2] == 11 && w[i + 3] == 15) {  // OpDecorate %v BuiltIn FragCoord
      var = w[i + 1];
      decorate_end = i + wc;
    }
    i += wc;
  }
  if (!var) return false;
  // 32-bit scalar constants (access chain indices).
  std::vector<std::pair<uint32_t, uint32_t>> constants;
  uint32_t ptr_type = 0, vec_type = 0, float_type = 0;
  size_t first_function = 0;
  for (size_t i = 5; i < n;) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (op == 43 && wc == 4) constants.emplace_back(w[i + 2], w[i + 3]);  // OpConstant
    if (op == 59 && wc >= 4 && w[i + 2] == var) ptr_type = w[i + 1];      // OpVariable
    if (op == 54 && !first_function) first_function = i;                // OpFunction
    i += wc;
  }
  auto constant_value = [&](uint32_t id, uint32_t& value) {
    for (const auto& c : constants) {
      if (c.first == id) {
        value = c.second;
        return true;
      }
    }
    return false;
  };
  // Pointers whose loads are scaled: the variable (vec4) and its x / y access chains (float).
  std::vector<uint32_t> xy_chains;
  for (size_t i = 5; i < n;) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if ((op == 65 || op == 66) && wc >= 4 && w[i + 3] == var) {  // OpAccessChain / OpInBoundsAccessChain
      uint32_t index = 0;
      if (wc != 5 || !constant_value(w[i + 4], index)) return false;
      if (index < 2) xy_chains.push_back(w[i + 2]);
    } else if ((op == 67 || op == 70 || op == 63) && wc >= 3) {  // OpPtrAccessChain / OpCopyMemory / OpStore
      for (uint32_t k = 1; k < wc; ++k) {
        if (w[i + k] == var) return false;
      }
    }
    i += wc;
  }
  for (size_t i = 5; i < n;) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (op == 32 && wc == 4 && w[i + 1] == ptr_type) vec_type = w[i + 3];  // OpTypePointer
    i += wc;
  }
  for (size_t i = 5; i < n;) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (op == 23 && wc == 4 && w[i + 1] == vec_type && w[i + 3] == 4) float_type = w[i + 2];  // OpTypeVector
    i += wc;
  }
  if (!ptr_type || !vec_type || !float_type || !first_function || !decorate_end) return false;
  uint32_t bound = w[3];
  const uint32_t scale_id = bound++, one_id = bound++, comp_id = bound++;
  uint32_t one_bits;
  const float one = 1.0f;
  std::memcpy(&one_bits, &one, 4);
  std::vector<uint32_t> out;
  out.reserve(n + 64);
  size_t loads = 0;
  out.insert(out.end(), w.begin(), w.begin() + 5);
  for (size_t i = 5; i < n;) {
    if (i == first_function) {
      out.insert(out.end(), {(4u << 16) | 50u, float_type, scale_id, one_bits});  // OpSpecConstant
      out.insert(out.end(), {(4u << 16) | 43u, float_type, one_id, one_bits});    // OpConstant
      out.insert(out.end(), {(7u << 16) | 51u, vec_type, comp_id, scale_id, scale_id, one_id, one_id});
    }
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    bool whole = false, component = false;
    if (op == 61 && wc >= 4) {  // OpLoad %type %r %pointer
      whole = w[i + 3] == var;
      for (uint32_t c : xy_chains) component = component || w[i + 3] == c;
    }
    out.insert(out.end(), w.begin() + i, w.begin() + i + wc);
    if (whole || component) {
      const uint32_t type = w[i + 1], result = w[i + 2], raw = bound++;
      out[out.size() - wc + 2] = raw;
      out.insert(out.end(), {(5u << 16) | 133u, type, result, raw, whole ? comp_id : scale_id});  // OpFMul
      ++loads;
    }
    i += wc;
    if (i == decorate_end) out.insert(out.end(), {(4u << 16) | 71u, scale_id, 1u, kFragCoordScaleSpecId});
  }
  if (!loads) return false;
  out[3] = bound;
  w.swap(out);
  return true;
}

}  // namespace

VulkanShader::VulkanTranslation::~VulkanTranslation() {
  if (shader_module_) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        static_cast<const VulkanShader&>(shader()).vulkan_device_;
    vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(), shader_module_,
                                                     nullptr);
  }
}

VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule() {
  if (!is_valid()) {
    return VK_NULL_HANDLE;
  }
  if (shader_module_ != VK_NULL_HANDLE) {
    return shader_module_;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(shader()).vulkan_device_;
  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = translated_binary().size();
  shader_module_create_info.pCode = reinterpret_cast<const uint32_t*>(translated_binary().data());
  std::vector<uint32_t> patched;
  if (g_fragcoord_scale_patch.load(std::memory_order_relaxed) &&
      shader().type() == xenos::ShaderType::kPixel && translated_binary().size() % 4 == 0) {
    patched.resize(translated_binary().size() / 4);
    std::memcpy(patched.data(), translated_binary().data(), translated_binary().size());
    if (PatchFragCoordScale(patched)) {
      shader_module_create_info.codeSize = patched.size() * 4;
      shader_module_create_info.pCode = patched.data();
    }
  }
  if (vulkan_device->functions().vkCreateShaderModule(vulkan_device->device(),
                                                      &shader_module_create_info, nullptr,
                                                      &shader_module_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanShader::VulkanTranslation: Failed to create a Vulkan shader "
        "module for shader {:016X} modification {:016X}",
        shader().ucode_data_hash(), modification());
    MakeInvalid();
    return VK_NULL_HANDLE;
  }
  return shader_module_;
}

VulkanShader::VulkanShader(const ui::vulkan::VulkanDevice* const vulkan_device,
                           const xenos::ShaderType shader_type, const uint64_t ucode_data_hash,
                           const uint32_t* const ucode_dwords, const size_t ucode_dword_count,
                           const std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      vulkan_device_(vulkan_device) {
  assert_not_null(vulkan_device);
}

Shader::Translation* VulkanShader::CreateTranslationInstance(uint64_t modification) {
  return new VulkanTranslation(*this, modification);
}

}  // namespace rex::graphics::vulkan
