// Copyright 2020 The libgav1 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/dsp/film_grain.h"
#include "src/utils/cpu.h"

#if LIBGAV1_TARGETING_SSE4_1
#include <smmintrin.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "src/dsp/common.h"
#include "src/dsp/constants.h"
#include "src/dsp/dsp.h"
#include "src/dsp/film_grain_common.h"
#include "src/dsp/x86/common_sse4.h"
#include "src/utils/common.h"
#include "src/utils/compiler_attributes.h"
#include "src/utils/logging.h"

namespace libgav1 {
namespace dsp {
namespace film_grain {
namespace {

// Load 8 values from source, widening to int16_t intermediate value size.
// The function is overloaded for each type and bitdepth for simplicity.
inline __m128i LoadSource(const int8_t* src) {
  return _mm_cvtepi8_epi16(LoadLo8(src));
}

// Load 8 values from source, widening to int16_t intermediate value size.
inline __m128i LoadSource(const uint8_t* src) {
  return _mm_cvtepu8_epi16(LoadLo8(src));
}

// Store 8 values to dest, narrowing to uint8_t from int16_t intermediate value.
inline void StoreUnsigned(uint8_t* dest, const __m128i data) {
  StoreLo8(dest, _mm_packus_epi16(data, data));
}

#if LIBGAV1_MAX_BITDEPTH >= 10
// Load 8 values from source.
inline __m128i LoadSource(const int16_t* src) { return LoadUnaligned16(src); }

// Load 8 values from source.
inline __m128i LoadSource(const uint16_t* src) { return LoadUnaligned16(src); }

// Store 8 values to dest.
inline void StoreUnsigned(uint16_t* dest, const __m128i data) {
  StoreUnaligned16(dest, data);
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

// For BlendNoiseWithImageChromaWithCfl, only |subsampling_x| is needed.
inline __m128i GetAverageLuma(const uint8_t* const luma, int subsampling_x) {
  if (subsampling_x != 0) {
    const __m128i src = LoadUnaligned16(luma);

    return RightShiftWithRounding_U16(
        _mm_hadd_epi16(_mm_cvtepu8_epi16(src),
                       _mm_unpackhi_epi8(src, _mm_setzero_si128())),
        1);
  }
  return _mm_cvtepu8_epi16(LoadLo8(luma));
}

#if LIBGAV1_MAX_BITDEPTH >= 10
// For BlendNoiseWithImageChromaWithCfl, only |subsampling_x| is needed.
inline __m128i GetAverageLuma(const uint16_t* const luma, int subsampling_x) {
  if (subsampling_x != 0) {
    return RightShiftWithRounding_U16(
        _mm_hadd_epi16(LoadUnaligned16(luma), LoadUnaligned16(luma + 8)), 1);
  }
  return LoadUnaligned16(luma);
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

inline __m128i Clip3(const __m128i value, const __m128i low,
                     const __m128i high) {
  const __m128i clipped_to_ceiling = _mm_min_epi16(high, value);
  return _mm_max_epi16(low, clipped_to_ceiling);
}

template <int bitdepth, typename Pixel>
inline __m128i GetScalingFactors(
    const uint8_t scaling_lut[kScalingLookupTableSize], const Pixel* source) {
  alignas(16) int16_t start_vals[8];
  if (bitdepth == 8) {
    // TODO(petersonab): Speed this up by creating a uint16_t scaling_lut.
    // Currently this code results in a series of movzbl.
    for (int i = 0; i < 8; ++i) {
      start_vals[i] = scaling_lut[source[i]];
    }
    return LoadAligned16(start_vals);
  }
  alignas(16) int16_t end_vals[8];
  // TODO(petersonab): Precompute this into a larger table for direct lookups.
  for (int i = 0; i < 8; ++i) {
    const int index = source[i] >> 2;
    start_vals[i] = scaling_lut[index];
    end_vals[i] = scaling_lut[index + 1];
  }
  const __m128i start = LoadAligned16(start_vals);
  const __m128i end = LoadAligned16(end_vals);
  __m128i remainder = LoadSource(source);
  remainder = _mm_srli_epi16(_mm_slli_epi16(remainder, 14), 1);
  const __m128i delta = _mm_mulhrs_epi16(_mm_sub_epi16(end, start), remainder);
  return _mm_add_epi16(start, delta);
}

// |scaling_shift| is in range [8,11].
template <int bitdepth>
inline __m128i ScaleNoise(const __m128i noise, const __m128i scaling,
                          const __m128i scaling_shift) {
  const __m128i shifted_scale_factors = _mm_sll_epi16(scaling, scaling_shift);
  return _mm_mulhrs_epi16(noise, shifted_scale_factors);
}

template <int bitdepth, typename GrainType, typename Pixel>
void BlendNoiseWithImageLuma_SSE4_1(
    const void* noise_image_ptr, int min_value, int max_luma, int scaling_shift,
    int width, int height, int start_height,
    const uint8_t scaling_lut_y[kScalingLookupTableSize],
    const void* source_plane_y, ptrdiff_t source_stride_y, void* dest_plane_y,
    ptrdiff_t dest_stride_y) {
  const auto* noise_image =
      static_cast<const Array2D<GrainType>*>(noise_image_ptr);
  const auto* in_y_row = static_cast<const Pixel*>(source_plane_y);
  source_stride_y /= sizeof(Pixel);
  auto* out_y_row = static_cast<Pixel*>(dest_plane_y);
  dest_stride_y /= sizeof(Pixel);
  const __m128i floor = _mm_set1_epi16(min_value);
  const __m128i ceiling = _mm_set1_epi16(max_luma);
  const int safe_width = width & ~7;
  const __m128i derived_scaling_shift = _mm_cvtsi32_si128(15 - scaling_shift);
  int y = 0;
  do {
    int x = 0;
    for (; x < safe_width; x += 8) {
      // TODO(b/133525232): Make 16-pixel version of loop body.
      const __m128i orig = LoadSource(&in_y_row[x]);
      const __m128i scaling =
          GetScalingFactors<bitdepth, Pixel>(scaling_lut_y, &in_y_row[x]);
      __m128i noise = LoadSource(&(noise_image[kPlaneY][y + start_height][x]));

      noise = ScaleNoise<bitdepth>(noise, scaling, derived_scaling_shift);
      const __m128i combined = _mm_add_epi16(orig, noise);
      StoreUnsigned(&out_y_row[x], Clip3(combined, floor, ceiling));
    }

    if (x < width) {
      Pixel luma_buffer[8];
      // Prevent arbitrary indices from entering GetScalingFactors.
      memset(luma_buffer, 0, sizeof(luma_buffer));
      const int valid_range = width - x;
      memcpy(luma_buffer, &in_y_row[x], valid_range * sizeof(in_y_row[0]));
      luma_buffer[valid_range] = in_y_row[width - 1];
      const __m128i orig = LoadSource(&in_y_row[x]);
      const __m128i scaling =
          GetScalingFactors<bitdepth, Pixel>(scaling_lut_y, luma_buffer);
      __m128i noise = LoadSource(&(noise_image[kPlaneY][y + start_height][x]));

      noise = ScaleNoise<bitdepth>(noise, scaling, derived_scaling_shift);
      const __m128i combined = _mm_add_epi16(orig, noise);
      StoreUnsigned(&out_y_row[x], Clip3(combined, floor, ceiling));
    }
    in_y_row += source_stride_y;
    out_y_row += dest_stride_y;
  } while (++y < height);
  out_y_row = static_cast<Pixel*>(dest_plane_y);
}

template <int bitdepth, typename GrainType, typename Pixel>
inline __m128i BlendChromaValsWithCfl(
    const Pixel* average_luma_buffer,
    const uint8_t scaling_lut[kScalingLookupTableSize],
    const Pixel* chroma_cursor, const GrainType* noise_image_cursor,
    const __m128i scaling_shift) {
  const __m128i scaling =
      GetScalingFactors<bitdepth, Pixel>(scaling_lut, average_luma_buffer);
  const __m128i orig = LoadSource(chroma_cursor);
  __m128i noise = LoadSource(noise_image_cursor);
  noise = ScaleNoise<bitdepth>(noise, scaling, scaling_shift);
  return _mm_add_epi16(orig, noise);
}

template <int bitdepth, typename GrainType, typename Pixel>
LIBGAV1_ALWAYS_INLINE void BlendChromaPlaneWithCfl_SSE4_1(
    const Array2D<GrainType>& noise_image, int min_value, int max_chroma,
    int width, int height, int start_height, int subsampling_x,
    int subsampling_y, int scaling_shift,
    const uint8_t scaling_lut[kScalingLookupTableSize], const Pixel* in_y_row,
    ptrdiff_t source_stride_y, const Pixel* in_chroma_row,
    ptrdiff_t source_stride_chroma, Pixel* out_chroma_row,
    ptrdiff_t dest_stride) {
  const __m128i floor = _mm_set1_epi16(min_value);
  const __m128i ceiling = _mm_set1_epi16(max_chroma);
  alignas(16) Pixel luma_buffer[16];

  const int chroma_height = (height + subsampling_y) >> subsampling_y;
  const int chroma_width = (width + subsampling_x) >> subsampling_x;
  const int safe_chroma_width = chroma_width & ~7;

  // Writing to this buffer avoids the cost of doing 8 lane lookups in a row
  // in GetScalingFactors.
  Pixel average_luma_buffer[8];
  assert(start_height % 2 == 0);
  start_height >>= subsampling_y;
  const __m128i derived_scaling_shift = _mm_cvtsi32_si128(15 - scaling_shift);
  int y = 0;
  do {
    int x = 0;
    for (; x < safe_chroma_width; x += 8) {
      const int luma_x = x << subsampling_x;
      // TODO(petersonab): Consider specializing by subsampling_x. In the 444
      // case &in_y_row[x] can be passed to GetScalingFactors directly.
      const __m128i average_luma =
          GetAverageLuma(&in_y_row[luma_x], subsampling_x);
      StoreUnsigned(average_luma_buffer, average_luma);

      const __m128i blended =
          BlendChromaValsWithCfl<bitdepth, GrainType, Pixel>(
              average_luma_buffer, scaling_lut, &in_chroma_row[x],
              &(noise_image[y + start_height][x]), derived_scaling_shift);
      StoreUnsigned(&out_chroma_row[x], Clip3(blended, floor, ceiling));
    }

    // This section only runs if width % (8 << sub_x) != 0. It should never run
    // on 720p and above.
    if (x < chroma_width) {
      // Prevent arbitrary indices from entering GetScalingFactors.
      memset(luma_buffer, 0, sizeof(luma_buffer));
      const int luma_x = x << subsampling_x;
      const int valid_range = width - luma_x;
      memcpy(luma_buffer, &in_y_row[luma_x], valid_range * sizeof(in_y_row[0]));
      luma_buffer[valid_range] = in_y_row[width - 1];
      const __m128i average_luma = GetAverageLuma(luma_buffer, subsampling_x);
      StoreUnsigned(average_luma_buffer, average_luma);

      const __m128i blended =
          BlendChromaValsWithCfl<bitdepth, GrainType, Pixel>(
              average_luma_buffer, scaling_lut, &in_chroma_row[x],
              &(noise_image[y + start_height][x]), derived_scaling_shift);
      StoreUnsigned(&out_chroma_row[x], Clip3(blended, floor, ceiling));
    }

    in_y_row += source_stride_y << subsampling_y;
    in_chroma_row += source_stride_chroma;
    out_chroma_row += dest_stride;
  } while (++y < chroma_height);
}

// This function is for the case params_.chroma_scaling_from_luma == true.
// This further implies that scaling_lut_u == scaling_lut_v == scaling_lut_y.
template <int bitdepth, typename GrainType, typename Pixel>
void BlendNoiseWithImageChromaWithCfl_SSE4_1(
    Plane plane, const FilmGrainParams& params, const void* noise_image_ptr,
    int min_value, int max_chroma, int width, int height, int start_height,
    int subsampling_x, int subsampling_y,
    const uint8_t scaling_lut[kScalingLookupTableSize],
    const void* source_plane_y, ptrdiff_t source_stride_y,
    const void* source_plane_uv, ptrdiff_t source_stride_uv,
    void* dest_plane_uv, ptrdiff_t dest_stride_uv) {
  const auto* noise_image =
      static_cast<const Array2D<GrainType>*>(noise_image_ptr);
  const auto* in_y = static_cast<const Pixel*>(source_plane_y);
  source_stride_y /= sizeof(Pixel);

  const auto* in_uv = static_cast<const Pixel*>(source_plane_uv);
  source_stride_uv /= sizeof(Pixel);
  auto* out_uv = static_cast<Pixel*>(dest_plane_uv);
  dest_stride_uv /= sizeof(Pixel);
  BlendChromaPlaneWithCfl_SSE4_1<bitdepth, GrainType, Pixel>(
      noise_image[plane], min_value, max_chroma, width, height, start_height,
      subsampling_x, subsampling_y, params.chroma_scaling, scaling_lut, in_y,
      source_stride_y, in_uv, source_stride_uv, out_uv, dest_stride_uv);
}

}  // namespace

namespace low_bitdepth {
namespace {

// |offset| is 32x4 packed to add with the result of _mm_madd_epi16.
inline __m128i BlendChromaValsNoCfl(
    const uint8_t scaling_lut[kScalingLookupTableSize],
    const uint8_t* chroma_cursor, const int8_t* noise_image_cursor,
    const __m128i& average_luma, const __m128i& scaling_shift,
    const __m128i& offset, const __m128i& weights) {
  uint8_t merged_buffer[8];
  const __m128i orig = LoadSource(chroma_cursor);
  const __m128i combined_lo =
      _mm_madd_epi16(_mm_unpacklo_epi16(average_luma, orig), weights);
  const __m128i combined_hi =
      _mm_madd_epi16(_mm_unpackhi_epi16(average_luma, orig), weights);
  const __m128i merged_base = _mm_packs_epi32(_mm_srai_epi32((combined_lo), 6),
                                              _mm_srai_epi32((combined_hi), 6));

  const __m128i merged = _mm_add_epi16(merged_base, offset);

  StoreLo8(merged_buffer, _mm_packus_epi16(merged, merged));
  const __m128i scaling =
      GetScalingFactors<8, uint8_t>(scaling_lut, merged_buffer);
  __m128i noise = LoadSource(noise_image_cursor);
  noise = ScaleNoise<8>(noise, scaling, scaling_shift);
  return _mm_add_epi16(orig, noise);
}

LIBGAV1_ALWAYS_INLINE void BlendChromaPlane8bpp_SSE4_1(
    const Array2D<int8_t>& noise_image, int min_value, int max_chroma,
    int width, int height, int start_height, int subsampling_x,
    int subsampling_y, int scaling_shift, int chroma_offset,
    int chroma_multiplier, int luma_multiplier,
    const uint8_t scaling_lut[kScalingLookupTableSize], const uint8_t* in_y_row,
    ptrdiff_t source_stride_y, const uint8_t* in_chroma_row,
    ptrdiff_t source_stride_chroma, uint8_t* out_chroma_row,
    ptrdiff_t dest_stride) {
  const __m128i floor = _mm_set1_epi16(min_value);
  const __m128i ceiling = _mm_set1_epi16(max_chroma);

  const int chroma_height = (height + subsampling_y) >> subsampling_y;
  const int chroma_width = (width + subsampling_x) >> subsampling_x;
  const int safe_chroma_width = chroma_width & ~7;
  alignas(16) uint8_t luma_buffer[16];
  const __m128i offset = _mm_set1_epi16(chroma_offset);
  const __m128i multipliers = _mm_set1_epi32(LeftShift(chroma_multiplier, 16) |
                                             (luma_multiplier & 0xFFFF));
  const __m128i derived_scaling_shift = _mm_cvtsi32_si128(15 - scaling_shift);

  start_height >>= subsampling_y;
  int y = 0;
  do {
    int x = 0;
    for (; x < safe_chroma_width; x += 8) {
      const int luma_x = x << subsampling_x;
      const __m128i average_luma =
          GetAverageLuma(&in_y_row[luma_x], subsampling_x);
      const __m128i blended = BlendChromaValsNoCfl(
          scaling_lut, &in_chroma_row[x], &(noise_image[y + start_height][x]),
          average_luma, derived_scaling_shift, offset, multipliers);
      StoreUnsigned(&out_chroma_row[x], Clip3(blended, floor, ceiling));
    }

    if (x < chroma_width) {
      // Prevent arbitrary indices from entering GetScalingFactors.
      memset(luma_buffer, 0, sizeof(luma_buffer));
      // TODO(b/174615556): Refactor BlendChromaValsNoCfl to accept pre-loaded
      // vector inputs so we can mask the chroma values here for msan.
      // Prevent uninitialized-value-error.
      uint8_t chroma_buffer[8];
#if LIBGAV1_MSAN
      memset(chroma_buffer, 0, sizeof(chroma_buffer));
#endif  // LIBGAV1_MSAN
      // Begin right edge iteration. Same as the normal iterations, but the
      // |average_luma| computation requires a duplicated luma value at the
      // end.
      const int luma_x = x << subsampling_x;
      const int valid_range = width - luma_x;
      memcpy(luma_buffer, &in_y_row[luma_x], valid_range * sizeof(in_y_row[0]));
      luma_buffer[valid_range] = in_y_row[width - 1];
      const int valid_range_chroma = chroma_width - x;
      memcpy(chroma_buffer, &in_chroma_row[x],
             valid_range_chroma * sizeof(in_chroma_row[0]));
      chroma_buffer[valid_range_chroma] = in_chroma_row[chroma_width - 1];

      const __m128i average_luma = GetAverageLuma(luma_buffer, subsampling_x);
      const __m128i blended = BlendChromaValsNoCfl(
          scaling_lut, chroma_buffer, &(noise_image[y + start_height][x]),
          average_luma, derived_scaling_shift, offset, multipliers);
      StoreUnsigned(&out_chroma_row[x], Clip3(blended, floor, ceiling));
      // End of right edge iteration.
    }

    in_y_row += source_stride_y << subsampling_y;
    in_chroma_row += source_stride_chroma;
    out_chroma_row += dest_stride;
  } while (++y < chroma_height);
}

// This function is for the case params_.chroma_scaling_from_luma == false.
void BlendNoiseWithImageChroma8bpp_SSE4_1(
    Plane plane, const FilmGrainParams& params, const void* noise_image_ptr,
    int min_value, int max_chroma, int width, int height, int start_height,
    int subsampling_x, int subsampling_y,
    const uint8_t scaling_lut[kScalingLookupTableSize],
    const void* source_plane_y, ptrdiff_t source_stride_y,
    const void* source_plane_uv, ptrdiff_t source_stride_uv,
    void* dest_plane_uv, ptrdiff_t dest_stride_uv) {
  assert(plane == kPlaneU || plane == kPlaneV);
  const auto* noise_image =
      static_cast<const Array2D<int8_t>*>(noise_image_ptr);
  const auto* in_y = static_cast<const uint8_t*>(source_plane_y);
  const auto* in_uv = static_cast<const uint8_t*>(source_plane_uv);
  auto* out_uv = static_cast<uint8_t*>(dest_plane_uv);

  const int offset = (plane == kPlaneU) ? params.u_offset : params.v_offset;
  const int luma_multiplier =
      (plane == kPlaneU) ? params.u_luma_multiplier : params.v_luma_multiplier;
  const int multiplier =
      (plane == kPlaneU) ? params.u_multiplier : params.v_multiplier;
  BlendChromaPlane8bpp_SSE4_1(
      noise_image[plane], min_value, max_chroma, width, height, start_height,
      subsampling_x, subsampling_y, params.chroma_scaling, offset, multiplier,
      luma_multiplier, scaling_lut, in_y, source_stride_y, in_uv,
      source_stride_uv, out_uv, dest_stride_uv);
}

void Init8bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(kBitdepth8);
  assert(dsp != nullptr);

  dsp->film_grain.blend_noise_luma =
      BlendNoiseWithImageLuma_SSE4_1<8, int8_t, uint8_t>;
  dsp->film_grain.blend_noise_chroma[0] = BlendNoiseWithImageChroma8bpp_SSE4_1;
  dsp->film_grain.blend_noise_chroma[1] =
      BlendNoiseWithImageChromaWithCfl_SSE4_1<8, int8_t, uint8_t>;
}

}  // namespace
}  // namespace low_bitdepth

#if LIBGAV1_MAX_BITDEPTH >= 10
namespace high_bitdepth {
namespace {

void Init10bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(kBitdepth10);
  assert(dsp != nullptr);

  dsp->film_grain.blend_noise_luma =
      BlendNoiseWithImageLuma_SSE4_1<10, int16_t, uint16_t>;
  dsp->film_grain.blend_noise_chroma[1] =
      BlendNoiseWithImageChromaWithCfl_SSE4_1<10, int16_t, uint16_t>;
}

}  // namespace
}  // namespace high_bitdepth
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

}  // namespace film_grain

void FilmGrainInit_SSE4_1() {
  film_grain::low_bitdepth::Init8bpp();
#if LIBGAV1_MAX_BITDEPTH >= 10
  film_grain::high_bitdepth::Init10bpp();
#endif  // LIBGAV1_MAX_BITDEPTH >= 10
}

}  // namespace dsp
}  // namespace libgav1

#else   // !LIBGAV1_ENABLE_SSE4_1

namespace libgav1 {
namespace dsp {

void FilmGrainInit_SSE4_1() {}

}  // namespace dsp
}  // namespace libgav1
#endif  // LIBGAV1_TARGETING_SSE4_1