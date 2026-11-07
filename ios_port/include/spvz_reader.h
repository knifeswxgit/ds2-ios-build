#pragma once
/**
 * DS2 iOS port - random-access reader for the DS2SPVZ1 compressed shader pack.
 *
 * The pack keeps the byte-address space of the original DS2SPVL1 stream, so the
 * shipped .idx sidecar (key -> virtual offset) is reused verbatim. Only the
 * blocks that contain a requested record are decompressed, into a small LRU.
 *
 * Layout produced by ios_port/tools/spvlib_compress.py:
 *   FileHeader <8s I I Q I I> : "DS2SPVZ1", 1, abi, original_size, block_target, block_count
 *   TOC        <Q Q I I Q Q>  : virtual_start, compressed_offset, csize, usize, 0, blake2b-8
 *   Blocks                    : zlib streams of whole records (80 B header + payload)
 *
 * Link against zlib (-lz, shipped with the iOS SDK). No exceptions, no threads
 * of its own: callers already serialize access to SpirvShaderLibrary::Restore.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <zlib.h>

namespace ds2_ios {

class SpvzReader {
 public:
  static constexpr size_t kHeaderSize = 32;
  // <Q Q I I Q Q> = vstart, coff, csize, usize, reserved, blake2b-8 checksum
  static constexpr size_t kTocEntrySize = 40;
  static constexpr size_t kCacheBlocks = 6;  // ~6 MiB of LRU at a 1 MiB block target

  ~SpvzReader() {
    if (file_) std::fclose(file_);
  }

  bool Open(const std::filesystem::path& path, uint32_t expected_abi) {
    if (file_) std::fclose(file_);
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) return false;
    uint8_t h[kHeaderSize];
    if (std::fread(h, 1, sizeof(h), file_) != sizeof(h)) return Fail();
    if (std::memcmp(h, "DS2SPVZ1", 8) != 0) return Fail();
    version_ = ReadU32(h + 8);
    abi_ = ReadU32(h + 12);
    original_size_ = ReadU64(h + 16);
    block_target_ = ReadU32(h + 24);
    const uint32_t count = ReadU32(h + 28);
    if (version_ != 1 || abi_ != expected_abi || !original_size_ || !count || count > 100000) return Fail();

    blocks_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t e[kTocEntrySize];
      if (std::fread(e, 1, sizeof(e), file_) != sizeof(e)) return Fail();
      Block& b = blocks_[i];
      b.vstart = ReadU64(e + 0);
      b.coff = ReadU64(e + 8);
      b.csize = ReadU32(e + 16);
      b.usize = ReadU32(e + 20);
      b.checksum = ReadU64(e + 32);  // e+24 is reserved
      if (!b.csize || !b.usize || b.usize > 64u * 1024u * 1024u ||
          (i && b.vstart != blocks_[i - 1].vstart + blocks_[i - 1].usize)) return Fail();
    }
    cache_.resize(kCacheBlocks);
    if (blocks_.front().vstart != 16 ||
        blocks_.back().vstart + blocks_.back().usize != original_size_) return Fail();

    // Rebuild the 16-byte DS2SPVL1 header so Open() can run its normal ABI check.
    WriteU32(header_ + 0, 0x53325344u);  // "DS2S"
    WriteU32(header_ + 4, 0x314C5650u);  // "PVL1"
    WriteU32(header_ + 8, 1);
    WriteU32(header_ + 12, abi_);
    return true;
  }

  uint64_t original_size() const { return original_size_; }
  const uint8_t* header() const { return header_; }
  bool valid() const { return file_ != nullptr; }

  /// Read `size` bytes at a virtual offset of the original .spvlib stream.
  bool Read(uint64_t off, void* dst, size_t size) {
    auto* out = static_cast<uint8_t*>(dst);
    if (off < 16) {
      const size_t n = std::min<size_t>(size, 16 - off);
      std::memcpy(out, header_ + off, n);
      out += n;
      off += n;
      size -= n;
      if (!size) return true;
    }
    while (size) {
      const size_t idx = FindBlock(off);
      if (idx == blocks_.size()) return false;
      const uint8_t* data = BlockData(idx);
      if (!data) return false;
      const Block& b = blocks_[idx];
      const uint64_t inner = off - b.vstart;
      const size_t avail = static_cast<size_t>(b.usize - inner);
      const size_t n = std::min<size_t>(size, avail);
      std::memcpy(out, data + inner, n);
      out += n;
      off += n;
      size -= n;
    }
    return true;
  }

 private:
  struct Block {
    uint64_t vstart, coff, checksum;
    uint32_t csize, usize;
  };
  struct CacheEntry {
    size_t index = static_cast<size_t>(-1);
    uint64_t last_use = 0;
    std::vector<uint8_t> data;
  };

  static uint32_t ReadU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }
  static uint64_t ReadU64(const uint8_t* p) {
    return uint64_t(ReadU32(p)) | uint64_t(ReadU32(p + 4)) << 32;
  }
  static void WriteU32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
  }
  bool Fail() {
    std::fclose(file_);
    file_ = nullptr;
    blocks_.clear();
    return false;
  }

  size_t FindBlock(uint64_t off) const {
    size_t lo = 0, hi = blocks_.size();
    while (lo + 1 < hi) {
      const size_t mid = (lo + hi) / 2;
      if (blocks_[mid].vstart <= off) lo = mid; else hi = mid;
    }
    if (lo < blocks_.size() && off < blocks_[lo].vstart + blocks_[lo].usize) return lo;
    return blocks_.size();
  }

  const uint8_t* BlockData(size_t idx) {
    for (auto& c : cache_) {
      if (c.index == idx) {
        c.last_use = ++tick_;
        return c.data.data();
      }
    }
    const Block& b = blocks_[idx];
    std::vector<uint8_t> packed(b.csize);
    if (std::fseek(file_, static_cast<long>(b.coff), SEEK_SET) ||
        std::fread(packed.data(), 1, packed.size(), file_) != packed.size()) return nullptr;
    std::vector<uint8_t> raw(b.usize);
    uLongf got = b.usize;
    if (uncompress(raw.data(), &got, packed.data(), packed.size()) != Z_OK || got != b.usize) return nullptr;

    size_t slot = 0;
    for (size_t i = 1; i < cache_.size(); ++i)
      if (cache_[i].last_use < cache_[slot].last_use) slot = i;
    cache_[slot].index = idx;
    cache_[slot].last_use = ++tick_;
    cache_[slot].data = std::move(raw);
    return cache_[slot].data.data();
  }

  FILE* file_ = nullptr;
  uint32_t version_ = 0, abi_ = 0, block_target_ = 0;
  uint64_t original_size_ = 0, tick_ = 0;
  uint8_t header_[16] = {};
  std::vector<Block> blocks_;
  std::vector<CacheEntry> cache_;
};

}  // namespace ds2_ios
