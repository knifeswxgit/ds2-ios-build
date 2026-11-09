// DS2 port: CPU decompression of the guest Xenos compressed (DXT) texture formats for hosts
// without BCn support (Metal on iOS has no S3TC; NVIDIA on Switch has all of them).
//
// The guest DXT bitstream is S3TC-compatible (the classic path uploads it verbatim into
// VK_FORMAT_BC1/2/3/4/5 images when the host supports them), but the Xenos GPU's own sampler
// semantics differ from S3TC hardware in a few corners (the 3-color DXT1 mode keeps the
// interpolated color opaque, and the 5->8 bit endpoint expansion is done in an overflow-safe
// "8 bits in 10" layout). This file is a literal CPU port of Xenia's reference decoders
// (src/xenia/gpu/shaders/pixel_formats.xesli and texture_load_dxt*.cs.xesl upstream), so the
// native renderer produces the same texels as the classic path's DXT load shaders - the ones
// already rendering the game on iOS.
//
// Input: one guest 4x4 block, 8 bytes for DXT1/DXT5A, 16 bytes for DXT2_3/DXT4_5/DXN, with the
// guest endianness already swapped to little-endian 32-bit words (the caller applies the same
// SwapElements the block-compressed upload path applies). Output: texels written with the given
// row stride, clipped writes are not needed (the caller pads rows to whole blocks and lets the
// buffer-to-image copy crop the extent).

#ifndef REXGPU_XENOS_NATIVE_BCN_DECODE_H_
#define REXGPU_XENOS_NATIVE_BCN_DECODE_H_

#include <cstdint>
#include <cstring>

#include <rex/graphics/xenos.h>

namespace rex::graphics::native {

inline uint32_t BcnLoad32LE(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8u) | (uint32_t(p[2]) << 16u) | (uint32_t(p[3]) << 24u);
}

// XeDXTColorEndpointsToBGR8In10: expands the two RGB565 endpoints of a 32-bit word (first in the
// low 16 bits, second in the high) to 8 bits each with 2 free bits between the packed channels
// (B in bits 0-9, G in 10-19, R in 20-29), so a x3 interpolation cannot overflow. Relative
// ordering of the endpoints is preserved (used for the DXT1 transparent-mode selection).
inline void BcnDXTColorEndpointsToBGR8In10(uint32_t bgr_end_565, uint32_t& ep0, uint32_t& ep1) {
  uint32_t x = ((bgr_end_565 << 3u) & (31u << 3u)) | ((bgr_end_565 << (12u - 5u)) & (63u << 12u)) |
               ((bgr_end_565 << (23u - 11u)) & (31u << 23u));
  uint32_t y = ((bgr_end_565 >> (16u - 3u)) & (31u << 3u)) | ((bgr_end_565 >> (21u - 12u)) & (63u << 12u)) |
               ((bgr_end_565 >> (27u - 23u)) & (31u << 23u));
  // Lower bit replication for the full 0-255 dynamic range (blue and red, then green).
  x |= (x >> 5u) & (7u | (7u << 20u));
  x |= (x >> 6u) & (3u << 10u);
  y |= (y >> 5u) & (7u | (7u << 20u));
  y |= (y >> 6u) & (3u << 10u);
  ep0 = x;
  ep1 = y;
}

// XeDXTHighColorWeights: sorts the color indices so they can be used as weights for the second
// endpoint (0 to 3); the first endpoint uses the bitwise NOT of the result.
inline uint32_t BcnDXTHighColorWeights(uint32_t codes) {
  codes = ((codes & 0x55555555u) << 1u) | ((codes & 0xAAAAAAAAu) >> 1u);
  return codes ^ ((codes & 0xAAAAAAAAu) >> 1u);
}

// XeDXTOpaqueRowToRGB8: 4 texels of one row of an opaque (DXT3/DXT5-style color) block.
inline void BcnDXTOpaqueRowToRGB8(uint32_t ep0, uint32_t ep1, uint32_t weights_high, uint32_t out[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t w = (weights_high >> (2u * i)) & 3u;
    const uint32_t v = ((~w) & 3u) * ep0 + (w & 3u) * ep1;
    out[i] = ((v >> 20u) / 3u) | ((((v >> 10u) & 1023u) / 3u) << 8u) | (((v & 1023u) / 3u) << 16u);
  }
}

// XeDXT1TransWeights: sorts the color indices of a transparent (punchthrough) DXT1 block.
inline uint32_t BcnDXT1TransWeights(uint32_t codes) {
  codes = ~codes;
  return codes ^ ((codes & 0x55555555u) << 1u);
}

// XeDXT1TransRowToRGBA8: 4 texels of one row of a transparent DXT1 block (3-color mode with
// 1-bit alpha: the half color stays opaque, the fourth index is fully transparent).
inline void BcnDXT1TransRowToRGBA8(uint32_t ep0, uint32_t ep1, uint32_t weights, uint32_t out[4]) {
  const uint32_t sums_log2 = weights & ((weights & 0xAAAAAAAAu) >> 1u);
  const uint32_t alpha_bits = (weights & 0x55555555u) | ((weights & 0xAAAAAAAAu) >> 1u);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t shift = 2u * i;
    const uint32_t lo = (weights >> shift) & 1u;
    const uint32_t hi = (weights >> (shift + 1u)) & 1u;
    const uint32_t v = lo * ep0 + hi * ep1;
    const uint32_t bgr_shift = (sums_log2 >> shift) & 1u;
    out[i] = (((v & 1023u) >> bgr_shift) << 16u) | ((((v >> 10u) & 1023u) >> bgr_shift) << 8u) |
             ((v >> 20u) >> bgr_shift) | (((alpha_bits >> shift) & 1u) * 0xFF000000u);
  }
}

// XeDXT5High8StepAlphaWeights: sorts half (24 bits, 8 texels) of the codes of a DXT5 alpha block
// so they can be used as weights for the second endpoint, 0 to 7, in the alpha0 > alpha1 mode.
inline uint32_t BcnDXT5High8StepAlphaWeights(uint32_t codes_24b) {
  const uint32_t is_first = ((codes_24b & 0x249249u) | ((codes_24b & 0x492492u) >> 1u) |
                             ((codes_24b & 0x924924u) >> 2u)) ^
                            0x249249u;
  const uint32_t is_second = (codes_24b & 0x249249u) & ~((codes_24b & 0x492492u) >> 1u) &
                             ~((codes_24b & 0x924924u) >> 2u);
  return ((codes_24b | is_first) - 0x249249u) | is_second | (is_second << 1u) | (is_second << 2u);
}

// XeDXT5High6StepAlphaWeights: same, 0 to 5, in the alpha0 <= alpha1 mode, with the 110/111
// codes kept as the 0 and 1 constants.
inline uint32_t BcnDXT5High6StepAlphaWeights(uint32_t codes_24b) {
  uint32_t is_constant = codes_24b & 0x492492u & ((codes_24b & 0x924924u) >> 1u);
  is_constant |= (is_constant << 1u) | (is_constant >> 1u);
  const uint32_t constant_values = ((codes_24b & 0x249249u) | (0x492492u | 0x924924u)) & is_constant;
  const uint32_t is_first = ((codes_24b & 0x249249u) | ((codes_24b & 0x492492u) >> 1u) |
                             ((codes_24b & 0x924924u) >> 2u)) ^
                            0x249249u;
  const uint32_t is_second = (codes_24b & 0x249249u) & ~((codes_24b & 0x492492u) >> 1u) &
                             ~((codes_24b & 0x924924u) >> 2u);
  codes_24b = ((codes_24b | is_first) - 0x249249u) | is_second | (is_second << 2u);
  return (codes_24b & ~is_constant) | constant_values;
}

inline uint32_t BcnDXT5HighAlphaWeights(uint32_t a0, uint32_t a1, uint32_t codes_24b) {
  return (a0 <= a1) ? BcnDXT5High6StepAlphaWeights(codes_24b) : BcnDXT5High8StepAlphaWeights(codes_24b);
}

// XeDXT58StepRowToA8: 4 alpha bytes of one row, alpha0 > alpha1 mode (12-bit weights, 4 texels).
inline uint32_t BcnDXT58StepRowToA8(uint32_t a0, uint32_t a1, uint32_t weights_high) {
  const uint32_t weights_low = ~weights_high;
  return ((a0 * (weights_low & 7u) + a1 * (weights_high & 7u)) / 7u) |
         (((a0 * ((weights_low >> 3u) & 7u) + a1 * ((weights_high >> 3u) & 7u)) / 7u) << 8u) |
         (((a0 * ((weights_low >> 6u) & 7u) + a1 * ((weights_high >> 6u) & 7u)) / 7u) << 16u) |
         (((a0 * ((weights_low >> 9u) & 7u) + a1 * ((weights_high >> 9u) & 7u)) / 7u) << 24u);
}

// XeDXT56StepRowToA8: 4 alpha bytes of one row, alpha0 <= alpha1 mode (with 0/1 constants).
inline uint32_t BcnDXT56StepRowToA8(uint32_t a0, uint32_t a1, uint32_t weights_6step) {
  uint32_t is_constant = weights_6step & 0x492u & ((weights_6step & 0x924u) >> 1u);
  is_constant |= (is_constant << 1u) | (is_constant >> 1u);
  const uint32_t weights_high = weights_6step & ~is_constant;
  const uint32_t weights_low = ((5u * 0x249u) - weights_high) & ~is_constant;
  const uint32_t row = ((a0 * (weights_low & 7u) + a1 * (weights_high & 7u)) / 5u) |
                       (((a0 * ((weights_low >> 3u) & 7u) + a1 * ((weights_high >> 3u) & 7u)) / 5u) << 8u) |
                       (((a0 * ((weights_low >> 6u) & 7u) + a1 * ((weights_high >> 6u) & 7u)) / 5u) << 16u) |
                       (((a0 * ((weights_low >> 9u) & 7u) + a1 * ((weights_high >> 9u) & 7u)) / 5u) << 24u);
  uint32_t constant_values = weights_6step & is_constant;
  constant_values = (constant_values & 1u) | ((constant_values & (1u << 3u)) << (8u - 3u)) |
                    ((constant_values & (1u << 6u)) << (16u - 6u)) |
                    ((constant_values & (1u << 9u)) << (24u - 9u));
  return row + constant_values * 0xFFu;
}

inline uint32_t BcnDXT5RowToA8(uint32_t a0, uint32_t a1, uint32_t weights) {
  return (a0 <= a1) ? BcnDXT56StepRowToA8(a0, a1, weights) : BcnDXT58StepRowToA8(a0, a1, weights);
}

// XeDXT5RowToA8In16: 4 alpha bytes of one row packed as two 16-bit parts (texel 0 and 1 in bits
// 0-7 / 16-23 of the first part, texels 2 and 3 in the second) - for DXN (two-channel) blocks.
inline void BcnDXT5RowToA8In16(uint32_t a0, uint32_t a1, uint32_t weights, uint32_t out[2]) {
  if (a0 <= a1) {
    uint32_t is_constant = weights & 0x492u & ((weights & 0x924u) >> 1u);
    is_constant |= (is_constant << 1u) | (is_constant >> 1u);
    const uint32_t weights_high = weights & ~is_constant;
    const uint32_t weights_low = ((5u * 0x249u) - weights_high) & ~is_constant;
    const uint32_t row0 = ((a0 * (weights_low & 7u) + a1 * (weights_high & 7u)) / 5u) |
                          (((a0 * ((weights_low >> 3u) & 7u) + a1 * ((weights_high >> 3u) & 7u)) / 5u) << 16u);
    const uint32_t row1 = ((a0 * ((weights_low >> 6u) & 7u) + a1 * ((weights_high >> 6u) & 7u)) / 5u) |
                          (((a0 * ((weights_low >> 9u) & 7u) + a1 * ((weights_high >> 9u) & 7u)) / 5u) << 16u);
    const uint32_t constant_weights = weights & is_constant;
    const uint32_t constant_x = (constant_weights & 1u) | ((constant_weights & (1u << 3u)) << (16u - 3u));
    const uint32_t constant_y = ((constant_weights >> 6u) & 1u) | ((constant_weights & (1u << 9u)) << (16u - 9u));
    out[0] = row0 + constant_x * 0xFFu;
    out[1] = row1 + constant_y * 0xFFu;
  } else {
    const uint32_t weights_low = ~weights;
    out[0] = ((a0 * (weights_low & 7u) + a1 * (weights & 7u)) / 7u) |
             (((a0 * ((weights_low >> 3u) & 7u) + a1 * ((weights >> 3u) & 7u)) / 7u) << 16u);
    out[1] = ((a0 * ((weights_low >> 6u) & 7u) + a1 * ((weights >> 6u) & 7u)) / 7u) |
             (((a0 * ((weights_low >> 9u) & 7u) + a1 * ((weights >> 9u) & 7u)) / 7u) << 16u);
  }
}

inline void BcnStoreRowRGBA8(uint8_t* dst, const uint32_t tex[4]) {
  std::memcpy(dst, tex, 16);
}

// DXT1 (8 bytes per block) to 4x4 RGBA8.
inline void BcnDecodeDXT1Block(const uint8_t* src, uint8_t* dst, size_t dst_row_bytes) {
  uint32_t ep0, ep1;
  BcnDXTColorEndpointsToBGR8In10(BcnLoad32LE(src), ep0, ep1);
  const uint32_t idx = BcnLoad32LE(src + 4);
  const bool trans = ep0 <= ep1;
  const uint32_t weights = trans ? BcnDXT1TransWeights(idx) : BcnDXTHighColorWeights(idx);
  for (uint32_t row = 0; row < 4; ++row) {
    const uint32_t w = weights >> (8u * row);
    uint32_t tex[4];
    if (trans) {
      BcnDXT1TransRowToRGBA8(ep0, ep1, w, tex);
    } else {
      BcnDXTOpaqueRowToRGB8(ep0, ep1, w, tex);
      for (uint32_t i = 0; i < 4; ++i) tex[i] |= 0xFF000000u;
    }
    BcnStoreRowRGBA8(dst + size_t(row) * dst_row_bytes, tex);
  }
}

// DXT2_3 (16 bytes per block: 8 bytes of explicit 4-bit alpha, then the DXT1-style color) to
// 4x4 RGBA8.
inline void BcnDecodeDXT3Block(const uint8_t* src, uint8_t* dst, size_t dst_row_bytes) {
  const uint32_t aw0 = BcnLoad32LE(src), aw1 = BcnLoad32LE(src + 4);
  uint32_t ep0, ep1;
  BcnDXTColorEndpointsToBGR8In10(BcnLoad32LE(src + 8), ep0, ep1);
  const uint32_t cweights = BcnDXTHighColorWeights(BcnLoad32LE(src + 12));
  for (uint32_t row = 0; row < 4; ++row) {
    uint32_t tex[4];
    BcnDXTOpaqueRowToRGB8(ep0, ep1, cweights >> (8u * row), tex);
    const uint32_t aw = (row & 2u) ? aw1 : aw0;
    const uint32_t ashift = (row & 1u) * 16u;
    for (uint32_t i = 0; i < 4; ++i) {
      tex[i] |= (((aw >> (ashift + 4u * i)) & 0xFu) * 0x11u) << 24u;
    }
    BcnStoreRowRGBA8(dst + size_t(row) * dst_row_bytes, tex);
  }
}

// DXT4_5 (16 bytes per block: 8 bytes of interpolated alpha, then the DXT1-style color) to 4x4
// RGBA8.
inline void BcnDecodeDXT5Block(const uint8_t* src, uint8_t* dst, size_t dst_row_bytes) {
  const uint32_t w0 = BcnLoad32LE(src), w1 = BcnLoad32LE(src + 4);
  const uint32_t a0 = w0 & 0xFFu, a1 = (w0 >> 8u) & 0xFFu;
  uint32_t ep0, ep1;
  BcnDXTColorEndpointsToBGR8In10(BcnLoad32LE(src + 8), ep0, ep1);
  const uint32_t cweights = BcnDXTHighColorWeights(BcnLoad32LE(src + 12));
  const uint32_t aw_lo = BcnDXT5HighAlphaWeights(a0, a1, (w0 >> 16u) | ((w1 & 0xFFu) << 16u));
  const uint32_t aw_hi = BcnDXT5HighAlphaWeights(a0, a1, w1 >> 8u);
  for (uint32_t row = 0; row < 4; ++row) {
    uint32_t tex[4];
    BcnDXTOpaqueRowToRGB8(ep0, ep1, cweights >> (8u * row), tex);
    const uint32_t alpha = BcnDXT5RowToA8(a0, a1, (row < 2u ? aw_lo : aw_hi) >> ((row & 1u) * 12u));
    for (uint32_t i = 0; i < 4; ++i) {
      tex[i] = (tex[i] & 0x00FFFFFFu) | (((alpha >> (8u * i)) & 0xFFu) << 24u);
    }
    BcnStoreRowRGBA8(dst + size_t(row) * dst_row_bytes, tex);
  }
}

// DXN (16 bytes per block: two BC4-style channels, red in bytes 0-7 and green in bytes 8-15) to
// 4x4 RG8.
inline void BcnDecodeDXNBlock(const uint8_t* src, uint8_t* dst, size_t dst_row_bytes) {
  const uint32_t w0 = BcnLoad32LE(src), w1 = BcnLoad32LE(src + 4);
  const uint32_t w2 = BcnLoad32LE(src + 8), w3 = BcnLoad32LE(src + 12);
  const uint32_t ra0 = w0 & 0xFFu, ra1 = (w0 >> 8u) & 0xFFu;
  const uint32_t ga0 = w2 & 0xFFu, ga1 = (w2 >> 8u) & 0xFFu;
  const uint32_t raw_lo = BcnDXT5HighAlphaWeights(ra0, ra1, (w0 >> 16u) | ((w1 & 0xFFu) << 16u));
  const uint32_t raw_hi = BcnDXT5HighAlphaWeights(ra0, ra1, w1 >> 8u);
  const uint32_t gaw_lo = BcnDXT5HighAlphaWeights(ga0, ga1, (w2 >> 16u) | ((w3 & 0xFFu) << 16u));
  const uint32_t gaw_hi = BcnDXT5HighAlphaWeights(ga0, ga1, w3 >> 8u);
  for (uint32_t row = 0; row < 4; ++row) {
    const uint32_t rw = (row < 2u ? raw_lo : raw_hi) >> ((row & 1u) * 12u);
    const uint32_t gw = (row < 2u ? gaw_lo : gaw_hi) >> ((row & 1u) * 12u);
    uint32_t rpx[2], gpx[2];
    BcnDXT5RowToA8In16(ra0, ra1, rw, rpx);
    BcnDXT5RowToA8In16(ga0, ga1, gw, gpx);
    const uint8_t rowout[8] = {uint8_t(rpx[0]),        uint8_t(gpx[0]),        uint8_t(rpx[0] >> 16),
                               uint8_t(gpx[0] >> 16),  uint8_t(rpx[1]),        uint8_t(gpx[1]),
                               uint8_t(rpx[1] >> 16),  uint8_t(gpx[1] >> 16)};
    std::memcpy(dst + size_t(row) * dst_row_bytes, rowout, 8);
  }
}

// DXT5A (8 bytes per block, BC4-style single channel) to 4x4 R8.
inline void BcnDecodeDXT5ABlock(const uint8_t* src, uint8_t* dst, size_t dst_row_bytes) {
  const uint32_t w0 = BcnLoad32LE(src), w1 = BcnLoad32LE(src + 4);
  const uint32_t a0 = w0 & 0xFFu, a1 = (w0 >> 8u) & 0xFFu;
  const uint32_t aw_lo = BcnDXT5HighAlphaWeights(a0, a1, (w0 >> 16u) | ((w1 & 0xFFu) << 16u));
  const uint32_t aw_hi = BcnDXT5HighAlphaWeights(a0, a1, w1 >> 8u);
  for (uint32_t row = 0; row < 4; ++row) {
    const uint32_t alpha = BcnDXT5RowToA8(a0, a1, (row < 2u ? aw_lo : aw_hi) >> ((row & 1u) * 12u));
    std::memcpy(dst + size_t(row) * dst_row_bytes, &alpha, 4);
  }
}

// Decodes one guest block of the given compressed format into the host texel layout. Returns
// false for formats without a CPU decoder (the caller falls back to the white texture).
inline bool BcnDecodeTextureBlock(xenos::TextureFormat format, const uint8_t* src, uint8_t* dst,
                                  size_t dst_row_bytes) {
  using T = xenos::TextureFormat;
  switch (format) {
    case T::k_DXT1:
      BcnDecodeDXT1Block(src, dst, dst_row_bytes);
      return true;
    case T::k_DXT2_3:
      BcnDecodeDXT3Block(src, dst, dst_row_bytes);
      return true;
    case T::k_DXT4_5:
      BcnDecodeDXT5Block(src, dst, dst_row_bytes);
      return true;
    case T::k_DXN:
      BcnDecodeDXNBlock(src, dst, dst_row_bytes);
      return true;
    case T::k_DXT5A:
      BcnDecodeDXT5ABlock(src, dst, dst_row_bytes);
      return true;
    default:
      return false;
  }
}

}  // namespace rex::graphics::native

#endif  // REXGPU_XENOS_NATIVE_BCN_DECODE_H_
