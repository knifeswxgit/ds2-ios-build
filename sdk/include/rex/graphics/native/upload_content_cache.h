#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rex::graphics::native {

// Frame-local immutable upload slices. Full byte comparison, not hash identity,
// permits reuse even when the guest changes the source memory in the same frame.
class UploadContentCache {
 public:
  UploadContentCache() { Reset(); }
  void Reset() { heads_.fill(-1); entries_.clear(); }
  bool Find(uint64_t hash, uint32_t tag, const void* data, size_t size,
            const uint8_t* mapping, uint64_t used, uint64_t& offset) const {
    for (int32_t i = heads_[hash % heads_.size()]; i >= 0; i = entries_[size_t(i)].next) {
      const auto& entry = entries_[size_t(i)];
      if (entry.hash == hash && entry.tag == tag && entry.size == size && entry.offset <= used &&
          size <= used - entry.offset && !std::memcmp(mapping + entry.offset, data, size)) {
        offset = entry.offset;
        return true;
      }
    }
    return false;
  }
  void Insert(uint64_t hash, uint32_t tag, size_t size, uint64_t offset) {
    if (entries_.size() >= 32768) return;
    auto& head = heads_[hash % heads_.size()];
    entries_.push_back({hash, offset, size, tag, head});
    head = int32_t(entries_.size() - 1);
  }
 private:
  struct Entry { uint64_t hash, offset; size_t size; uint32_t tag; int32_t next; };
  std::array<int32_t, 4096> heads_;
  std::vector<Entry> entries_;
};

}  // namespace rex::graphics::native
