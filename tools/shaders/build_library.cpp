#include <array>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/pipeline/shader/spirv_library.h>
#include <rex/graphics/pipeline/shader/native_vertex_layout.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;

struct Input {
  std::unique_ptr<SpirvShader> shader;
  uint32_t input_mask = 0;
};
struct Verification {
  size_t input;
  uint64_t modification;
  uint64_t binary_hash;
};

bool SameBindings(const SpirvShader& a, const SpirvShader& b) {
  const auto& at = a.GetTextureBindingsAfterTranslation();
  const auto& bt = b.GetTextureBindingsAfterTranslation();
  const auto& as = a.GetSamplerBindingsAfterTranslation();
  const auto& bs = b.GetSamplerBindingsAfterTranslation();
  if (at.size() != bt.size() || as.size() != bs.size() ||
      a.GetUsedTextureMaskAfterTranslation() != b.GetUsedTextureMaskAfterTranslation()) return false;
  for (size_t i = 0; i < at.size(); ++i) {
    if (at[i].fetch_constant != bt[i].fetch_constant || at[i].dimension != bt[i].dimension ||
        at[i].is_signed != bt[i].is_signed) return false;
  }
  for (size_t i = 0; i < as.size(); ++i) {
    if (as[i].fetch_constant != bs[i].fetch_constant || as[i].mag_filter != bs[i].mag_filter ||
        as[i].min_filter != bs[i].min_filter || as[i].mip_filter != bs[i].mip_filter ||
        as[i].aniso_filter != bs[i].aniso_filter) return false;
  }
  return true;
}

int SelfTest(const std::filesystem::path& directory) {
  if (std::filesystem::exists(directory) || !std::filesystem::create_directory(directory)) return 2;
  const uint32_t code[] = {1, 0x2000, 0};
  const uint64_t hash = XXH3_64bits(code, sizeof(code));
  SpirvShader shader(rex::graphics::xenos::ShaderType::kPixel, hash, code, 3, std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), false, false, false);
  auto* translation = shader.GetOrCreateTranslation(0);
  if (!translator.TranslateAnalyzedShader(*translation)) return 1;
  const auto path = directory / "valid.spvlib";
  FILE* output = std::fopen(path.c_str(), "wb");
  if (!output || !SpirvShaderLibrary::Begin(output) ||
      !SpirvShaderLibrary::Append(output, *translation, translator.ShaderLibrarySignature()) || fclose(output)) return 1;
  SpirvShaderLibrary library;
  if (!library.Open(path) || library.size() != 1) return 1;
  SpirvShader restored(shader.type(), hash, code, 3, std::endian::native);
  auto* restored_translation = restored.GetOrCreateTranslation(0);
  if (library.Restore(*restored_translation, translator.ShaderLibrarySignature() ^ 1) ||
      restored_translation->is_translated()) return 1;
  if (!library.Restore(*restored_translation, translator.ShaderLibrarySignature()) ||
      restored_translation->translated_binary() != translation->translated_binary() ||
      !SameBindings(shader, restored)) return 1;
  auto index_path = path;
  index_path += ".idx";
  if (!library.WriteIndex(index_path)) return 1;
  SpirvShaderLibrary indexed;
  if (!indexed.Open(path) || !indexed.indexed() || indexed.size() != 1) return 1;
  SpirvShader fast(shader.type(), hash, code, 3, std::endian::native);
  auto* fast_translation = fast.GetOrCreateTranslation(0);
  if (!indexed.Restore(*fast_translation, translator.ShaderLibrarySignature()) ||
      fast_translation->translated_binary() != translation->translated_binary() ||
      !SameBindings(shader, fast)) return 1;
  { std::fstream index(index_path, std::ios::binary | std::ios::in | std::ios::out);
    index.seekp(-1, std::ios::end); index.put('\x7f'); }
  SpirvShaderLibrary bad_index;
  if (!bad_index.Open(path) || bad_index.indexed() || bad_index.size() != 1) return 1;
  SpirvShader fallback(shader.type(), hash, code, 3, std::endian::native);
  auto* fallback_translation = fallback.GetOrCreateTranslation(0);
  if (!bad_index.Restore(*fallback_translation, translator.ShaderLibrarySignature()) ||
      fallback_translation->translated_binary() != translation->translated_binary()) return 1;
  std::filesystem::resize_file(index_path, 8);
  SpirvShaderLibrary short_index;
  if (!short_index.Open(path) || short_index.indexed() || short_index.size() != 1) return 1;
  // An internally consistent but stale index must never restore a different record.
  std::filesystem::remove(index_path);
  if (!library.WriteIndex(index_path)) return 1;
  {
    std::fstream index(index_path, std::ios::binary | std::ios::in | std::ios::out);
    std::array<uint64_t, 6> header;
    std::array<uint64_t, 5> entry;
    index.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    index.read(reinterpret_cast<char*>(entry.data()), sizeof(entry));
    ++entry[4];
    header[4] = XXH3_64bits(entry.data(), sizeof(entry));
    index.seekp(0);
    index.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
    index.write(reinterpret_cast<const char*>(entry.data()), sizeof(entry));
  }
  SpirvShaderLibrary stale_index;
  if (!stale_index.Open(path) || !stale_index.indexed()) return 1;
  SpirvShader stale(shader.type(), hash, code, 3, std::endian::native);
  auto* stale_translation = stale.GetOrCreateTranslation(0);
  if (stale_index.Restore(*stale_translation, translator.ShaderLibrarySignature()) ||
      stale_translation->is_translated()) return 1;
  std::ifstream input(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  const auto corrupt = directory / "corrupt.spvlib";
  bytes.back() ^= 1;
  { std::ofstream file(corrupt, std::ios::binary); file.write(bytes.data(), bytes.size()); }
  SpirvShaderLibrary damaged;
  if (!damaged.Open(corrupt)) return 1;
  SpirvShader rejected(shader.type(), hash, code, 3, std::endian::native);
  auto* rejected_translation = rejected.GetOrCreateTranslation(0);
  if (damaged.Restore(*rejected_translation, translator.ShaderLibrarySignature()) ||
      rejected_translation->is_translated()) return 1;
  const auto truncated = directory / "truncated.spvlib";
  { std::ofstream file(truncated, std::ios::binary); file.write(bytes.data(), bytes.size() / 2); }
  SpirvShaderLibrary incomplete;
  if (incomplete.Open(truncated)) return 1;
  std::puts("selftest passed: roundtrip, indexed restore, corrupt/truncated index fallback, stale index rejection, signature mismatch, corrupt payload, truncated archive");
  return 0;
}

int NativeTemplateTest(const std::filesystem::path& manifest_path, const std::filesystem::path& output) {
  if (std::filesystem::exists(output) || !std::filesystem::create_directory(output)) return 2;
  std::ifstream manifest(manifest_path);
  std::string stage, name;
  size_t tested = 0;
  rex::string::StringBuffer disassembly;
  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), false, false, false);
  while (manifest >> stage >> name) {
    if (stage != "vs") continue;
    std::ifstream file(manifest_path.parent_path() / name, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size <= 0 || size % 4) return 2;
    std::vector<uint32_t> words(size_t(size) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(words.data()), size);
    for (auto& word : words) word = __builtin_bswap32(word);
    uint32_t bound = uint32_t(words.size() / 3);
    for (uint32_t i = 0; i < bound; ++i) {
      rex::graphics::ucode::ControlFlowInstruction cf[2];
      rex::graphics::ucode::UnpackControlFlowInstructions(words.data() + i * 3, cf);
      for (const auto& instruction : cf) {
        if (!rex::graphics::ucode::IsControlFlowOpcodeExec(instruction.opcode())) continue;
        bound = std::min(bound, instruction.exec.address());
        uint32_t sequence = instruction.exec.sequence();
        for (uint32_t j = 0; j < instruction.exec.count(); ++j, sequence >>= 2) {
          const size_t address = size_t(instruction.exec.address() + j) * 3;
          if (address + 3 > words.size()) return 2;
          if (!(sequence & 1)) continue;
          const auto& fetch = *reinterpret_cast<const rex::graphics::ucode::FetchInstruction*>(words.data() + address);
          if (fetch.opcode() == rex::graphics::ucode::FetchOpcode::kVertexFetch && !fetch.vertex_fetch().is_mini_fetch()) {
            // Test input only: simulate a valid runtime stride patch, not a game-library variant.
            words[address + 2] = (words[address + 2] & ~255u) | 64u;
          }
        }
      }
    }
    SpirvShader shader(rex::graphics::xenos::ShaderType::kVertex, 0, words.data(), words.size(), std::endian::native);
    shader.AnalyzeUcode(disassembly);
    const auto layout = BuildNativeVertexLayout(shader);
    if (!layout.valid) continue;
    SpirvShaderTranslator::Modification modification(translator.GetDefaultVertexShaderModification(shader.GetDynamicAddressableRegisterCount(0)));
    modification.vertex.native_vertex_inputs = 1;
    modification.vertex.interpolator_mask = shader.writes_interpolators();
    auto* translation = shader.GetOrCreateTranslation(modification.value);
    if (!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid()) return 1;
    const auto binary = output / (std::to_string(tested) + ".spv");
    std::ofstream spv(binary, std::ios::binary);
    spv.write(reinterpret_cast<const char*>(translation->translated_binary().data()), translation->translated_binary().size());
    ++tested;
  }
  std::printf("native template translations=%zu (test-only stride patch, not runtime coverage)\n", tested);
  return tested ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--native-test") return NativeTemplateTest(argv[2], argv[3]);
  if (argc == 3 && std::string(argv[1]) == "--selftest") return SelfTest(argv[2]);
  if (argc == 4 && std::string(argv[1]) == "--disasm") {
    // --disasm <ps|vs> <raw ucode dwords, host order>: prints the microcode disassembly.
    FILE* f = std::fopen(argv[3], "rb");
    if (!f) return 1;
    std::vector<uint32_t> code;
    uint32_t w;
    while (std::fread(&w, 4, 1, f) == 1) code.push_back(w);
    std::fclose(f);
    const uint64_t hash = XXH3_64bits(code.data(), code.size() * 4);
    SpirvShader shader(std::string(argv[2]) == "vs" ? rex::graphics::xenos::ShaderType::kVertex
                                                   : rex::graphics::xenos::ShaderType::kPixel,
                       hash, code.data(), uint32_t(code.size()), std::endian::native);
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    std::printf("%s\n", shader.ucode_disassembly().c_str());
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--inspect") {
    SpirvShaderLibrary library;
    if (!library.Open(argv[2])) return 1;
    std::printf("entries=%zu indexed=%d\n", library.size(), library.indexed() ? 1 : 0);
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "--index") {
    SpirvShaderLibrary library;
    if (!library.Open(argv[2]) || !library.WriteIndex(argv[3])) return 1;
    std::printf("indexed %zu variants\n", library.size());
    return 0;
  }
  // RC65: --learned <ds2_shader_cache.spvlib>: also generate, for every source shader, the modification patterns
  // the console actually used (runtime-learned cache), adapted to the shader (register count, interpolators,
  // early-Z eligibility). The RC60/61 pack never matched them (0 hits).
  std::vector<std::pair<uint32_t, uint64_t>> learned_patterns;  // (shader type, modification)
  // RC71: several --learned caches; the microcode stored in them is also added as source shaders (the console
  // meets shaders the old dump manifest does not have).
  struct CachedSource {
    uint32_t type;
    uint64_t hash;
    std::vector<uint32_t> dwords;  // host order
  };
  std::vector<CachedSource> cached_sources;
  {
    std::set<std::pair<uint32_t, uint64_t>> unique;
    std::set<std::pair<uint64_t, uint32_t>> unique_sources;
    while (argc >= 5 && std::string(argv[1]) == "--learned") {
      FILE* f = std::fopen(argv[2], "rb");
      if (!f) return 2;
      std::array<uint32_t, 4> header;
      if (std::fread(header.data(), 1, sizeof(header), f) != sizeof(header)) return 2;
      for (;;) {
        std::array<uint64_t, 10> r;
        if (std::fread(r.data(), 1, sizeof(r), f) != sizeof(r)) break;
        const uint64_t payload = r[4] * 4 + r[5] + (r[6] + r[7]) * 4;
        if (r[3] > 1 || payload > (8u << 20) || !r[4] || r[4] > 262144) break;
        unique.emplace(uint32_t(r[3]), r[1]);
        std::vector<uint32_t> dwords(static_cast<size_t>(r[4]));
        if (std::fread(dwords.data(), 4, dwords.size(), f) != dwords.size()) break;
        if (unique_sources.emplace(r[0], uint32_t(r[3])).second) {
          cached_sources.push_back({uint32_t(r[3]), r[0], std::move(dwords)});
        }
        if (std::fseek(f, long(payload - r[4] * 4), SEEK_CUR)) break;
      }
      std::fclose(f);
      argv += 2;
      argc -= 2;
    }
    learned_patterns.assign(unique.begin(), unique.end());
    if (!learned_patterns.empty()) {
      std::printf("learned patterns: %zu, cached source shaders: %zu\n", learned_patterns.size(),
                  cached_sources.size());
    }
  }
  if (argc != 3) {
    std::fprintf(stderr, "usage: rex-shader-library [--learned <cache.spvlib>] <manifest.txt> <new-output.spvlib>\n");
    return 2;
  }
  const std::filesystem::path manifest_path(argv[1]);
  const std::filesystem::path output_path(argv[2]);
  if (std::filesystem::exists(output_path)) {
    std::fprintf(stderr, "refusing to overwrite %s\n", argv[2]);
    return 2;
  }
  std::ifstream manifest(manifest_path);
  if (!manifest) return 2;
  std::vector<Input> inputs;
  std::set<uint32_t> masks{0};
  std::set<std::pair<uint64_t, uint32_t>> seen;
  size_t source_count = 0;
  std::string stage, name;
  rex::string::StringBuffer disassembly;
  while (manifest >> stage >> name) {
    if (stage != "vs" && stage != "ps") return 2;
    std::ifstream file(manifest_path.parent_path() / name, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size <= 0 || size > (1 << 20) || size % 4) return 2;
    std::vector<uint32_t> code(size_t(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) return 2;
    const uint64_t hash = XXH3_64bits(code.data(), size_t(size));
    const auto type = stage == "vs" ? rex::graphics::xenos::ShaderType::kVertex
                                     : rex::graphics::xenos::ShaderType::kPixel;
    ++source_count;
    if (!seen.emplace(hash, uint32_t(type)).second) continue;
    Input input;
    input.shader = std::make_unique<SpirvShader>(type, hash, code.data(), code.size(), std::endian::big);
    input.shader->AnalyzeUcode(disassembly);
    if (type == rex::graphics::xenos::ShaderType::kPixel) {
      uint32_t param_gen;
      input.input_mask = input.shader->GetInterpolatorInputMask(reg::SQ_PROGRAM_CNTL{0}, reg::SQ_CONTEXT_MISC{0}, param_gen);
      masks.insert(input.input_mask);
    }
    inputs.push_back(std::move(input));
  }
  size_t cached_added = 0;
  for (auto& cs : cached_sources) {
    const auto type = cs.type ? rex::graphics::xenos::ShaderType::kPixel : rex::graphics::xenos::ShaderType::kVertex;
    if (!seen.emplace(cs.hash, uint32_t(type)).second) continue;
    Input input;
    input.shader = std::make_unique<SpirvShader>(type, cs.hash, cs.dwords.data(), cs.dwords.size(),
                                                 std::endian::native);
    input.shader->AnalyzeUcode(disassembly);
    if (type == rex::graphics::xenos::ShaderType::kPixel) {
      uint32_t param_gen;
      input.input_mask = input.shader->GetInterpolatorInputMask(reg::SQ_PROGRAM_CNTL{0}, reg::SQ_CONTEXT_MISC{0}, param_gen);
      masks.insert(input.input_mask);
    }
    inputs.push_back(std::move(input));
    ++cached_added;
  }
  if (cached_added) std::printf("source shaders added from the caches: %zu\n", cached_added);
  if (inputs.empty()) return 2;
  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), false, false, false);
  // RC96: REX_FOLD_TEXTURE_SIGNS=1 builds the pack with the texture signs as specialization constants
  // (for ds2_native_fold_texture_signs = true).
  if (const char* fold = std::getenv("REX_FOLD_TEXTURE_SIGNS"); fold && fold[0] == '1') {
    translator.set_fold_texture_signs(true);
    std::printf("texture signs: specialization constants\n");
  }
  const uint64_t signature = translator.ShaderLibrarySignature();
  FILE* output = std::fopen(argv[2], "wb");
  if (!output || !SpirvShaderLibrary::Begin(output)) return 2;
  std::vector<Verification> checks;
  size_t failed_variants = 0, failed_shaders = 0, native_sources = 0;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto& input = inputs[i];
    auto& shader = *input.shader;
    const bool vertex = shader.type() == rex::graphics::xenos::ShaderType::kVertex;
    std::set<uint64_t> modifications;
    const uint32_t register_count = shader.GetDynamicAddressableRegisterCount(0);
    const uint32_t own_mask = vertex ? shader.writes_interpolators() : input.input_mask;
    std::set<uint32_t> shader_masks{own_mask, 0};
    for (uint32_t mask : masks) shader_masks.insert(mask & own_mask);
    for (uint32_t mask : shader_masks) {
      if (vertex) {
        for (auto type : {Shader::HostVertexShaderType::kVertex,
                          Shader::HostVertexShaderType::kPointListAsTriangleStrip,
                          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip}) {
          SpirvShaderTranslator::Modification mod(translator.GetDefaultVertexShaderModification(register_count, type));
          mod.vertex.interpolator_mask = mask;
          modifications.insert(mod.value);
        }
      } else {
        SpirvShaderTranslator::Modification mod(translator.GetDefaultPixelShaderModification(register_count));
        mod.pixel.interpolator_mask = mask;
        modifications.insert(mod.value);
        if (shader.implicit_early_z_write_allowed()) {
          mod.pixel.depth_stencil_mode = SpirvShaderTranslator::Modification::DepthStencilMode::kEarlyHint;
          modifications.insert(mod.value);
        }
      }
    }
    if (vertex && BuildNativeVertexLayout(shader).valid) {
      ++native_sources;
      SpirvShaderTranslator::Modification mod(translator.GetDefaultVertexShaderModification(register_count));
      mod.vertex.interpolator_mask = own_mask;
      mod.vertex.native_vertex_inputs = 1;
      modifications.insert(mod.value);
    } else if (!vertex) {
      SpirvShaderTranslator::Modification mod(translator.GetDefaultPixelShaderModification(register_count));
      mod.pixel.interpolator_mask = own_mask;
      mod.pixel.alpha_test_always = 1;
      modifications.insert(mod.value);
    }
    // With learned patterns, keep only them and the default variant (the generic RC60 enumeration never hit).
    if (!learned_patterns.empty()) {
      modifications.clear();
      modifications.insert(vertex ? translator.GetDefaultVertexShaderModification(register_count)
                                  : translator.GetDefaultPixelShaderModification(register_count));
    }
    for (const auto& [type, pattern] : learned_patterns) {
      if (type != uint32_t(shader.type())) continue;
      SpirvShaderTranslator::Modification mod(pattern);
      if (vertex) {
        mod.vertex.dynamic_addressable_register_count = register_count;
        mod.vertex.interpolator_mask &= own_mask;
        mod.vertex.native_vertex_inputs = 0;
      } else {
        mod.pixel.dynamic_addressable_register_count = register_count;
        mod.pixel.interpolator_mask &= own_mask;
        mod.pixel.interpolators_centroid &= own_mask;
        if (!shader.implicit_early_z_write_allowed() &&
            mod.pixel.depth_stencil_mode == SpirvShaderTranslator::Modification::DepthStencilMode::kEarlyHint) {
          mod.pixel.depth_stencil_mode = SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
        }
      }
      modifications.insert(mod.value);
    }
    bool shader_ok = false;
    for (uint64_t modification : modifications) {
      Shader::Translation& translation = *shader.GetOrCreateTranslation(modification);
      if (!translator.TranslateAnalyzedShader(translation) || !translation.is_valid() ||
          translation.translated_binary().empty()) {
        ++failed_variants;
        std::fprintf(stderr, "failed %016llX/%016llX\n", (unsigned long long)shader.ucode_data_hash(),
                     (unsigned long long)modification);
        continue;
      }
      if (!SpirvShaderLibrary::Append(output, translation, signature)) {
        fclose(output);
        return 2;
      }
      checks.push_back({i, modification,
                       XXH3_64bits(translation.translated_binary().data(), translation.translated_binary().size())});
      shader_ok = true;
      shader.DestroyTranslation(modification);
    }
    if (!shader_ok) ++failed_shaders;
    if ((i + 1) % 100 == 0) {
      std::printf("translated %zu/%zu unique shaders, %zu variants\n", i + 1, inputs.size(), checks.size());
      std::fflush(stdout);
    }
  }
  if (fclose(output)) return 2;
  SpirvShaderLibrary library;
  if (!library.Open(output_path)) return 2;
  for (const auto& check : checks) {
    auto& original = *inputs[check.input].shader;
    SpirvShader restored(original.type(), original.ucode_data_hash(), original.ucode_dwords(),
                          original.ucode_dword_count(), std::endian::native);
    auto* translation = restored.GetOrCreateTranslation(check.modification);
    if (!library.Restore(*translation, signature) ||
        XXH3_64bits(translation->translated_binary().data(), translation->translated_binary().size()) != check.binary_hash ||
        !SameBindings(restored, original)) {
      std::fprintf(stderr, "library verification failed\n");
      return 2;
    }
  }
  std::printf("source_containers=%zu unique_shaders=%zu failed_shaders=%zu variants=%zu failed_variants=%zu verified=%zu signature=%016llX\n",
              source_count, inputs.size(), failed_shaders, checks.size(), failed_variants, checks.size(),
              (unsigned long long)signature);
  std::printf("native_vertex_eligible_source_shaders=%zu\n", native_sources);
  return failed_shaders ? 1 : 0;
}
