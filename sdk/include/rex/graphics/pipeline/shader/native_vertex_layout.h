#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>

namespace rex::graphics {

// Raw integer attributes retain the translator's exact endian/format conversion.
struct NativeVertexLayout {
  struct Binding { uint32_t fetch_constant, stride; };
  struct Word { uint32_t binding, offset; };
  std::vector<Binding> bindings;
  std::vector<Word> words;
  bool valid = false;

  int Location(uint32_t fetch_constant, uint32_t offset) const {
    for (size_t i = 0; i < words.size(); ++i) {
      if (bindings[words[i].binding].fetch_constant == fetch_constant && words[i].offset == offset) return int(i);
    }
    return -1;
  }
};

inline NativeVertexLayout BuildNativeVertexLayout(const Shader& shader) {
  NativeVertexLayout layout;
  if (shader.type() != xenos::ShaderType::kVertex || shader.vertex_bindings().empty() ||
      shader.vertex_bindings().size() > 8 || shader.memexport_eM_written() || !shader.label_addresses().empty()) return layout;
  // Only unconditional fetches before the first arithmetic/control-flow instruction.
  // Parsing the SDK's own disassembly is conservative: unknown syntax falls back.
  const std::string& disassembly = shader.ucode_disassembly();
  const size_t last_fetch = disassembly.rfind("vfetch_");
  if (last_fetch == std::string::npos || disassembly.find("loop") != std::string::npos ||
      disassembly.find("jmp") != std::string::npos || disassembly.find("call") != std::string::npos) return layout;
  size_t pos = 0;
  while (pos <= last_fetch) {
    const size_t end = disassembly.find('\n', pos);
    std::string line = disassembly.substr(pos, end == std::string::npos ? end : end - pos);
    const size_t comment = line.find("*/");
    if (comment != std::string::npos) line.erase(0, comment + 2);
    const size_t first = line.find_first_not_of(" \t\r");
    if (first != std::string::npos) {
      line.erase(0, first);
      const std::string opcode = line.substr(0, line.find_first_of(" \t\r"));
      if (opcode != "exec" && opcode != "exece" && opcode != "alloc" && opcode != "cnop" &&
          opcode != "serialize" && opcode != "vfetch_full" && opcode != "vfetch_mini") return layout;
      if (line.find("Predicate") != std::string::npos || line.find("pred") != std::string::npos) return layout;
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  for (const auto& binding : shader.vertex_bindings()) {
    for (const auto& attribute : binding.attributes) {
      const auto& fetch = attribute.fetch_instr;
      const uint32_t fc = fetch.operands[1].storage_index;
      const uint32_t stride = fetch.attributes.stride;
      if (fc >= 96 || !stride || stride > 512) return layout;
      uint32_t binding_index = 0;
      while (binding_index < layout.bindings.size() && layout.bindings[binding_index].fetch_constant != fc) ++binding_index;
      if (binding_index == layout.bindings.size()) {
        layout.bindings.push_back({fc, stride * 4});
        if (layout.bindings.size() > 8) return layout;
      } else if (layout.bindings[binding_index].stride != stride * 4) {
        return layout;
      }
      const auto& index = fetch.operands[0];
      if (fetch.is_predicated || fetch.attributes.offset < 0 ||
          index.storage_source != InstructionStorageSource::kRegister || index.storage_index != 0 ||
          index.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
          index.GetComponent(0) != SwizzleSource::kX || index.is_negated || index.is_absolute_value ||
          (fetch.result.storage_target == InstructionStorageTarget::kRegister &&
           (fetch.result.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
            (fetch.result.storage_index == 0 && (fetch.result.GetUsedWriteMask() & 1))))) return layout;
      uint32_t mask = xenos::GetVertexFormatNeededWords(fetch.attributes.data_format,
                                                        fetch.result.GetUsedResultComponents());
      for (uint32_t word = 0; word < 4; ++word) {
        if (!(mask & (1u << word))) continue;
        const uint32_t offset = uint32_t(fetch.attributes.offset) + word;
        if (offset >= stride) return layout;
        if (layout.Location(fc, offset * 4) < 0) {
          layout.words.push_back({binding_index, offset * 4});
          if (layout.words.size() > 32) return layout;
        }
      }
    }
  }
  layout.valid = !layout.words.empty();
  return layout;
}

}  // namespace rex::graphics
