#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <map>

#include <rex/platform.h>
#if REX_PLATFORM_IOS
#include <memory>
#include "spvz_reader.h"
#endif

#include <rex/graphics/pipeline/shader/spirv.h>

namespace rex::graphics {

class SpirvShaderLibrary {
 public:
  ~SpirvShaderLibrary();
  // tolerate_tail: stop at a truncated/corrupt record instead of failing (the runtime-learned cache can be cut
  // short by a power-off); valid_size() is where the valid records end.
  bool Open(const std::filesystem::path& path, uint64_t expected_signature = 0, bool tolerate_tail = false);
  uint64_t valid_size() const { return valid_size_; }
  // RC68: with tolerate_tail the file is opened for update when possible (Horizon refuses a second handle that
  // writes to a file already open), so the learned cache appends through this same handle.
  FILE* writable_file() const { return writable_ ? file_ : nullptr; }
  bool Contains(uint64_t ucode_hash, uint64_t modification, uint64_t signature, uint64_t type) const {
    return entries_.count(Key{ucode_hash, modification, signature, type}) != 0;
  }
  bool Restore(Shader::Translation& translation, uint64_t signature);
  bool WriteIndex(const std::filesystem::path& path) const;
  bool indexed() const { return indexed_; }
  const char* last_failure() const { return last_failure_; }
  static bool Begin(FILE* file);
  static bool Append(FILE* file, const Shader::Translation& translation, uint64_t signature);
  size_t size() const { return entries_.size(); }

 private:
  using Key = std::array<uint64_t, 4>;
  bool ReadIndex(const std::filesystem::path& path);
  FILE* file_ = nullptr;
  bool writable_ = false;
  uint64_t file_size_ = 0;
  uint64_t valid_size_ = 0;
  bool indexed_ = false;
  uint64_t file_signature_ = 0;
  const char* last_failure_ = "key not found";
  std::map<Key, uint64_t> entries_;
#if REX_PLATFORM_IOS
  // DS2SPVZ1: random-access compressed pack, ~3.3x smaller than the raw
  // .spvlib. The .idx offsets still address the original (virtual) stream.
  std::unique_ptr<ds2_ios::SpvzReader> spvz_;
#endif
};

bool RestoreConfiguredSpirvLibrary(Shader::Translation& translation, uint64_t signature);
// RC64: runtime-learned cache (ds2_shader_cache): every translation made on the console is appended, and restored
// on the next start instead of being translated again.
void RecordConfiguredSpirvCache(const Shader::Translation& translation, uint64_t signature);

}  // namespace rex::graphics
