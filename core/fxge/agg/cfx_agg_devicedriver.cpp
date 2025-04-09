// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fxge/agg/cfx_agg_devicedriver.h"

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <utility>

#include "build/build_config.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/notreached.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxcrt/unowned_ptr_exclusion.h"
#include "core/fxcrt/zip.h"
#include "core/fxge/agg/cfx_agg_cliprgn.h"
#include "core/fxge/agg/cfx_agg_imagerenderer.h"
#include "core/fxge/cfx_defaultrenderdevice.h"
#include "core/fxge/cfx_graphstatedata.h"
#include "core/fxge/cfx_path.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/dib/cfx_imagestretcher.h"

// Ignore fallthrough warnings in agg23 headers.
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "third_party/agg23/agg_clip_liang_barsky.h"
#include "third_party/agg23/agg_conv_dash.h"
#include "third_party/agg23/agg_conv_stroke.h"
#include "third_party/agg23/agg_curves.h"
#include "third_party/agg23/agg_path_storage.h"
#include "third_party/agg23/agg_pixfmt_gray.h"
#include "third_party/agg23/agg_rasterizer_scanline_aa.h"
#include "third_party/agg23/agg_renderer_scanline.h"
#include "third_party/agg23/agg_scanline_u.h"
#if defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace pdfium {
namespace {

const float kMaxPos = 32000.0f;

CFX_PointF HardClip(const CFX_PointF& pos) {
  return CFX_PointF(std::clamp(pos.x, -kMaxPos, kMaxPos),
                    std::clamp(pos.y, -kMaxPos, kMaxPos));
}

template <typename T>
void DoAlphaMerge(T& pixel, int src_r, int src_g, int src_b, int src_alpha) {
  pixel.red = FXDIB_ALPHA_MERGE(pixel.red, src_r, src_alpha);
  pixel.green = FXDIB_ALPHA_MERGE(pixel.green, src_g, src_alpha);
  pixel.blue = FXDIB_ALPHA_MERGE(pixel.blue, src_b, src_alpha);
}

void RgbByteOrderCompositeRect(const RetainPtr<CFX_DIBitmap>& bitmap,
                               int left,
                               int top,
                               int width,
                               int height,
                               FX_ARGB src_argb) {
  int src_alpha = FXARGB_A(src_argb);
  if (src_alpha == 0) {
    return;
  }

  FX_RECT rect(left, top, left + width, top + height);
  rect.Intersect(0, 0, bitmap->GetWidth(), bitmap->GetHeight());
  width = rect.Width();
  const int src_r = FXARGB_R(src_argb);
  const int src_g = FXARGB_G(src_argb);
  const int src_b = FXARGB_B(src_argb);
  const int bytes_per_pixel = bitmap->GetBPP() / 8;
  if (src_alpha == 255) {
    if (bytes_per_pixel == 4) {
      const int src_abgr = FXARGB_TOBGRORDERDIB(src_argb);
      for (int row = rect.top; row < rect.bottom; row++) {
        auto dest_row_span = bitmap->GetWritableScanlineAs<uint32_t>(row);
        fxcrt::Fill(dest_row_span.subspan(rect.left, width), src_abgr);
      }
      return;
    }

    for (int row = rect.top; row < rect.bottom; row++) {
      auto dest_row_span =
          bitmap->GetWritableScanlineAs<FX_RGB_STRUCT<uint8_t>>(row);
      for (auto& rgb : dest_row_span.subspan(rect.left, width)) {
        rgb.red = src_r;
        rgb.green = src_g;
        rgb.blue = src_b;
      }
    }
    return;
  }

  if (bitmap->IsAlphaFormat()) {
    for (int row = rect.top; row < rect.bottom; row++) {
      auto dest_row_span =
          bitmap->GetWritableScanlineAs<FX_RGBA_STRUCT<uint8_t>>(row);
      for (auto& rgba : dest_row_span.subspan(rect.left, width)) {
        if (rgba.alpha == 0) {
          rgba.red = src_r;
          rgba.green = src_g;
          rgba.blue = src_b;
          rgba.alpha = src_alpha;
          continue;
        }

        const uint8_t dest_alpha =
            rgba.alpha + src_alpha - rgba.alpha * src_alpha / 255;
        const int alpha_ratio = src_alpha * 255 / dest_alpha;
        DoAlphaMerge(rgba, src_r, src_g, src_b, alpha_ratio);
      }
    }
    return;
  }

  if (bytes_per_pixel == 4) {
    for (int row = rect.top; row < rect.bottom; row++) {
      auto dest_row_span =
          bitmap->GetWritableScanlineAs<FX_RGBA_STRUCT<uint8_t>>(row);
      for (auto& rgba : dest_row_span.subspan(rect.left, width)) {
        DoAlphaMerge(rgba, src_r, src_g, src_b, src_alpha);
      }
    }
    return;
  }

  for (int row = rect.top; row < rect.bottom; row++) {
    auto dest_row_span =
        bitmap->GetWritableScanlineAs<FX_RGB_STRUCT<uint8_t>>(row);
    for (auto& rgb : dest_row_span.subspan(rect.left, width)) {
      DoAlphaMerge(rgb, src_r, src_g, src_b, src_alpha);
    }
  }
}

void RgbByteOrderTransferBitmap(RetainPtr<CFX_DIBitmap> pBitmap,
                                int width,
                                int height,
                                RetainPtr<const CFX_DIBBase> pSrcBitmap,
                                int src_left,
                                int src_top) {
  int dest_left = 0;
  int dest_top = 0;
  if (!pBitmap->GetOverlapRect(dest_left, dest_top, width, height,
                               pSrcBitmap->GetWidth(), pSrcBitmap->GetHeight(),
                               src_left, src_top, nullptr)) {
    return;
  }

  const FXDIB_Format dest_format = pBitmap->GetFormat();
  const FXDIB_Format src_format = pSrcBitmap->GetFormat();

  if (dest_format == src_format) {
    if (pBitmap->GetBPP() == 32) {
      for (int row = 0; row < height; row++) {
        int dest_row = dest_top + row;
        auto dest_scan =
            pBitmap->GetWritableScanlineAs<FX_RGBA_STRUCT<uint8_t>>(dest_row)
                .subspan(dest_left);
        int src_row = src_top + row;
        auto src_scan =
            pSrcBitmap->GetScanlineAs<FX_BGRA_STRUCT<uint8_t>>(src_row).subspan(
                src_left, width);
        for (auto [input, output] : fxcrt::Zip(src_scan, dest_scan)) {
          output.red = input.red;
          output.green = input.green;
          output.blue = input.blue;
          output.alpha = input.alpha;
        }
      }
      return;
    }

    CHECK_EQ(FXDIB_Format::kBgr, src_format);
    for (int row = 0; row < height; row++) {
      int dest_row = dest_top + row;
      auto dest_scan =
          pBitmap->GetWritableScanlineAs<FX_RGB_STRUCT<uint8_t>>(dest_row)
              .subspan(dest_left);
      int src_row = src_top + row;
      auto src_scan =
          pSrcBitmap->GetScanlineAs<FX_BGR_STRUCT<uint8_t>>(src_row).subspan(
              src_left, width);
      for (auto [input, output] : fxcrt::Zip(src_scan, dest_scan)) {
        output.red = input.red;
        output.green = input.green;
        output.blue = input.blue;
      }
    }
    return;
  }

  if (dest_format == FXDIB_Format::kBgr) {
    CHECK_EQ(src_format, FXDIB_Format::kBgrx);
    for (int row = 0; row < height; row++) {
      int dest_row = dest_top + row;
      auto dest_scan =
          pBitmap->GetWritableScanlineAs<FX_RGB_STRUCT<uint8_t>>(dest_row)
              .subspan(dest_left);
      int src_row = src_top + row;
      auto src_scan =
          pSrcBitmap->GetScanlineAs<FX_BGRA_STRUCT<uint8_t>>(src_row).subspan(
              src_left, width);
      for (auto [input, output] : fxcrt::Zip(src_scan, dest_scan)) {
        output.red = input.red;
        output.green = input.green;
        output.blue = input.blue;
      }
    }
    return;
  }

  CHECK(dest_format == FXDIB_Format::kBgra ||
        dest_format == FXDIB_Format::kBgrx);
  if (src_format == FXDIB_Format::kBgr) {
    for (int row = 0; row < height; row++) {
      int dest_row = dest_top + row;
      auto dest_scan =
          pBitmap->GetWritableScanlineAs<FX_RGBA_STRUCT<uint8_t>>(dest_row)
              .subspan(dest_left);
      int src_row = src_top + row;
      auto src_scan =
          pSrcBitmap->GetScanlineAs<FX_BGR_STRUCT<uint8_t>>(src_row).subspan(
              src_left, width);
      for (auto [input, output] : fxcrt::Zip(src_scan, dest_scan)) {
        output.red = input.red;
        output.green = input.green;
        output.blue = input.blue;
        output.alpha = 255;
      }
    }
    return;
  }
  if (src_format != FXDIB_Format::kBgrx) {
    return;
  }
  CHECK_EQ(dest_format, FXDIB_Format::kBgra);
  for (int row = 0; row < height; row++) {
    int dest_row = dest_top + row;
    auto dest_scan =
        pBitmap->GetWritableScanlineAs<FX_RGBA_STRUCT<uint8_t>>(dest_row)
            .subspan(dest_left);
    int src_row = src_top + row;
    auto src_scan =
        pSrcBitmap->GetScanlineAs<FX_BGRA_STRUCT<uint8_t>>(src_row).subspan(
            src_left, width);
    for (auto [input, output] : fxcrt::Zip(src_scan, dest_scan)) {
      output.red = input.red;
      output.green = input.green;
      output.blue = input.blue;
      output.alpha = 255;
    }
  }
}

void RasterizeStroke(agg::rasterizer_scanline_aa* rasterizer,
                     agg::path_storage* path_data,
                     const CFX_Matrix* pObject2Device,
                     const CFX_GraphStateData* pGraphState,
                     float scale,
                     bool bTextMode) {
  agg::line_cap_e cap;
  switch (pGraphState->line_cap()) {
    case CFX_GraphStateData::LineCap::kRound:
      cap = agg::round_cap;
      break;
    case CFX_GraphStateData::LineCap::kSquare:
      cap = agg::square_cap;
      break;
    default:
      cap = agg::butt_cap;
      break;
  }
  agg::line_join_e join;
  switch (pGraphState->line_join()) {
    case CFX_GraphStateData::LineJoin::kRound:
      join = agg::round_join;
      break;
    case CFX_GraphStateData::LineJoin::kBevel:
      join = agg::bevel_join;
      break;
    default:
      join = agg::miter_join_revert;
      break;
  }
  float width = pGraphState->line_width() * scale;
  float unit = 1.0f;
  if (pObject2Device) {
    unit =
        1.0f / ((pObject2Device->GetXUnit() + pObject2Device->GetYUnit()) / 2);
  }
  width = std::max(width, unit);
  const std::vector<float>& dash_array = pGraphState->dash_array();
  if (!dash_array.empty()) {
    using DashConverter = agg::conv_dash<agg::path_storage>;
    DashConverter dash(*path_data);
    for (size_t i = 0; i < (dash_array.size() + 1) / 2; i++) {
      float on = dash_array[i * 2];
      if (on <= 0.000001f) {
        on = 0.1f;
      }
      float off = i * 2 + 1 == dash_array.size() ? on : dash_array[i * 2 + 1];
      off = std::max(off, 0.0f);
      dash.add_dash(fabs(on * scale), fabs(off * scale));
    }
    dash.dash_start(pGraphState->dash_phase() * scale);
    using DashStroke = agg::conv_stroke<DashConverter>;
    DashStroke stroke(dash);
    stroke.line_join(join);
    stroke.line_cap(cap);
    stroke.miter_limit(pGraphState->miter_limit());
    stroke.width(width);
    rasterizer->add_path_transformed(stroke, pObject2Device);
    return;
  }
  agg::conv_stroke<agg::path_storage> stroke(*path_data);
  stroke.line_join(join);
  stroke.line_cap(cap);
  stroke.miter_limit(pGraphState->miter_limit());
  stroke.width(width);
  rasterizer->add_path_transformed(stroke, pObject2Device);
}

agg::filling_rule_e GetAlternateOrWindingFillType(
    const CFX_FillRenderOptions& fill_options) {
  return fill_options.fill_type == CFX_FillRenderOptions::FillType::kWinding
             ? agg::fill_non_zero
             : agg::fill_even_odd;
}

RetainPtr<CFX_DIBitmap> GetClipMaskFromRegion(const CFX_AggClipRgn* r) {
  return (r && r->GetType() == CFX_AggClipRgn::kMaskF) ? r->GetMask() : nullptr;
}

FX_RECT GetClipBoxFromRegion(const RetainPtr<CFX_DIBitmap>& device,
                             const CFX_AggClipRgn* region) {
  if (region) {
    return region->GetBox();
  }
  return FX_RECT(0, 0, device->GetWidth(), device->GetHeight());
}

class CFX_AggRenderer {
 public:
  CFX_AggRenderer(const RetainPtr<CFX_DIBitmap>& pDevice,
                  const RetainPtr<CFX_DIBitmap>& pBackdropDevice,
                  const CFX_AggClipRgn* pClipRgn,
                  uint32_t color,
                  bool bFullCover,
                  bool bRgbByteOrder);

  // Needed for agg caller
  void prepare(unsigned) {}

  template <class Scanline>
  void render(const Scanline& sl);

 private:
  using CompositeSpanFunc = void (CFX_AggRenderer::*)(uint8_t*,
                                                      int,
                                                      int,
                                                      int,
                                                      const uint8_t*,
                                                      const uint8_t*);

  void CompositeSpan(uint8_t* dest_scan,
                     const uint8_t* backdrop_scan,
                     int bytes_per_pixel,
                     bool bDestAlpha,
                     int col_start,
                     int col_end,
                     const uint8_t* cover_scan,
                     const uint8_t* clip_scan);

  void CompositeSpanGray(uint8_t* dest_scan,
                         int bytes_per_pixel,
                         int col_start,
                         int col_end,
                         const uint8_t* cover_scan,
                         const uint8_t* clip_scan);

  void CompositeSpanARGB(uint8_t* dest_scan,
                         int bytes_per_pixel,
                         int col_start,
                         int col_end,
                         const uint8_t* cover_scan,
                         const uint8_t* clip_scan);

  void CompositeSpanRGB(uint8_t* dest_scan,
                        int bytes_per_pixel,
                        int col_start,
                        int col_end,
                        const uint8_t* cover_scan,
                        const uint8_t* clip_scan);

  static CompositeSpanFunc GetCompositeSpanFunc(
      const RetainPtr<CFX_DIBitmap>& device) {
    CHECK_NE(device->GetBPP(), 1);
    if (device->GetBPP() == 8) {
      return &CFX_AggRenderer::CompositeSpanGray;
    }
    const FXDIB_Format format = device->GetFormat();
    if (format == FXDIB_Format::kBgra) {
      return &CFX_AggRenderer::CompositeSpanARGB;
    }
    CHECK(format == FXDIB_Format::kBgr || format == FXDIB_Format::kBgrx);
    return &CFX_AggRenderer::CompositeSpanRGB;
  }

  inline int GetSrcAlpha(const uint8_t* clip_scan, int col) const {
    return clip_scan ? alpha_ * UNSAFE_TODO(clip_scan[col]) / 255 : alpha_;
  }

  inline int GetSourceAlpha(const uint8_t* cover_scan,
                            const uint8_t* clip_scan,
                            int col) const {
    return UNSAFE_TODO(clip_scan ? alpha_ * cover_scan[col] * clip_scan[col] /
                                       255 / 255
                                 : alpha_ * cover_scan[col] / 255);
  }

  static int GetColStart(int span_left, int clip_left) {
    return span_left < clip_left ? clip_left - span_left : 0;
  }

  static int GetColEnd(int span_left, int span_len, int clip_right) {
    return span_left + span_len < clip_right ? span_len
                                             : clip_right - span_left;
  }

  const FX_BGR_STRUCT<uint8_t>& GetBGR() const {
    return std::get<FX_BGR_STRUCT<uint8_t>>(color_data_);
  }
  int GetGray() const { return std::get<int>(color_data_); }

  const int alpha_;
  std::variant<FX_BGR_STRUCT<uint8_t>, int> color_data_;
  const uint32_t color_;
  const bool full_cover_;
  const bool rgb_byte_order_;
  const FX_RECT clip_box_;
  RetainPtr<CFX_DIBitmap> const backdrop_device_;
  RetainPtr<CFX_DIBitmap> const clip_mask_;
  RetainPtr<CFX_DIBitmap> const device_;
  UnownedPtr<const CFX_AggClipRgn> clip_rgn_;
  const CompositeSpanFunc composite_span_func_;
};

void CFX_AggRenderer::CompositeSpan(uint8_t* dest_scan,
                                    const uint8_t* backdrop_scan,
                                    int bytes_per_pixel,
                                    bool bDestAlpha,
                                    int col_start,
                                    int col_end,
                                    const uint8_t* cover_scan,
                                    const uint8_t* clip_scan) {
  CHECK(bytes_per_pixel);
  UNSAFE_TODO({
    dest_scan += col_start * bytes_per_pixel;
    backdrop_scan += col_start * bytes_per_pixel;
    if (rgb_byte_order_) {
      if (bytes_per_pixel == 4 && bDestAlpha) {
        const auto& bgr = GetBGR();
        for (int col = col_start; col < col_end; col++) {
          int src_alpha = GetSrcAlpha(clip_scan, col);
          uint8_t dest_alpha =
              backdrop_scan[3] + src_alpha - backdrop_scan[3] * src_alpha / 255;
          dest_scan[3] = dest_alpha;
          int alpha_ratio = src_alpha * 255 / dest_alpha;
          if (full_cover_) {
            *dest_scan++ =
                FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.red, alpha_ratio);
            *dest_scan++ =
                FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.green, alpha_ratio);
            *dest_scan++ =
                FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.blue, alpha_ratio);
            dest_scan++;
            backdrop_scan++;
          } else {
            int r = FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.red, alpha_ratio);
            int g = FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.green, alpha_ratio);
            int b = FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.blue, alpha_ratio);
            backdrop_scan++;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, r, cover_scan[col]);
            dest_scan++;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, g, cover_scan[col]);
            dest_scan++;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, b, cover_scan[col]);
            dest_scan += 2;
          }
        }
        return;
      }
      if (bytes_per_pixel == 3 || bytes_per_pixel == 4) {
        const auto& bgr = GetBGR();
        for (int col = col_start; col < col_end; col++) {
          int src_alpha = GetSrcAlpha(clip_scan, col);
          int r = FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.red, src_alpha);
          int g = FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.green, src_alpha);
          int b = FXDIB_ALPHA_MERGE(*backdrop_scan, bgr.blue, src_alpha);
          backdrop_scan += bytes_per_pixel - 2;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, r, cover_scan[col]);
          dest_scan++;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, g, cover_scan[col]);
          dest_scan++;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, b, cover_scan[col]);
          dest_scan += bytes_per_pixel - 2;
        }
      }
      return;
    }
    if (bytes_per_pixel == 4 && bDestAlpha) {
      const auto& bgr = GetBGR();
      for (int col = col_start; col < col_end; col++) {
        int src_alpha = GetSrcAlpha(clip_scan, col);
        int src_alpha_covered = src_alpha * cover_scan[col] / 255;
        if (src_alpha_covered == 0) {
          dest_scan += 4;
          continue;
        }
        if (cover_scan[col] == 255) {
          dest_scan[3] = src_alpha_covered;
          *dest_scan++ = bgr.blue;
          *dest_scan++ = bgr.green;
          *dest_scan = bgr.red;
          dest_scan += 2;
          continue;
        }
        if (dest_scan[3] == 0) {
          dest_scan[3] = src_alpha_covered;
          *dest_scan++ = bgr.blue;
          *dest_scan++ = bgr.green;
          *dest_scan = bgr.red;
          dest_scan += 2;
          continue;
        }
        uint8_t cover = cover_scan[col];
        dest_scan[3] = FXDIB_ALPHA_MERGE(dest_scan[3], src_alpha, cover);
        *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.blue, cover);
        dest_scan++;
        *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.green, cover);
        dest_scan++;
        *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.red, cover);
        dest_scan += 2;
      }
      return;
    }
    if (bytes_per_pixel == 3 || bytes_per_pixel == 4) {
      const auto& bgr = GetBGR();
      for (int col = col_start; col < col_end; col++) {
        int src_alpha = GetSrcAlpha(clip_scan, col);
        if (full_cover_) {
          *dest_scan++ =
              FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.blue, src_alpha);
          *dest_scan++ =
              FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.green, src_alpha);
          *dest_scan = FXDIB_ALPHA_MERGE(*backdrop_scan, bgr.red, src_alpha);
          dest_scan += bytes_per_pixel - 2;
          backdrop_scan += bytes_per_pixel - 2;
          continue;
        }
        int b = FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.blue, src_alpha);
        int g = FXDIB_ALPHA_MERGE(*backdrop_scan++, bgr.green, src_alpha);
        int r = FXDIB_ALPHA_MERGE(*backdrop_scan, bgr.red, src_alpha);
        backdrop_scan += bytes_per_pixel - 2;
        *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, b, cover_scan[col]);
        dest_scan++;
        *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, g, cover_scan[col]);
        dest_scan++;
        *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, r, cover_scan[col]);
        dest_scan += bytes_per_pixel - 2;
      }
      return;
    }
    CHECK_EQ(bytes_per_pixel, 1);
    const int gray = GetGray();
    for (int col = col_start; col < col_end; col++) {
      int src_alpha = GetSrcAlpha(clip_scan, col);
      if (full_cover_) {
        *dest_scan = FXDIB_ALPHA_MERGE(*backdrop_scan++, gray, src_alpha);
        continue;
      }
      int gray_merged = FXDIB_ALPHA_MERGE(*backdrop_scan++, gray, src_alpha);
      *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, gray_merged, cover_scan[col]);
      dest_scan++;
    }
  });
}

void CFX_AggRenderer::CompositeSpanGray(uint8_t* dest_scan,
                                        int bytes_per_pixel,
                                        int col_start,
                                        int col_end,
                                        const uint8_t* cover_scan,
                                        const uint8_t* clip_scan) {
  DCHECK(!rgb_byte_order_);
  const int gray = GetGray();
  UNSAFE_TODO({
    dest_scan += col_start;
    for (int col = col_start; col < col_end; col++) {
      int src_alpha = GetSourceAlpha(cover_scan, clip_scan, col);
      if (src_alpha) {
        if (src_alpha == 255) {
          *dest_scan = gray;
        } else {
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, gray, src_alpha);
        }
      }
      dest_scan++;
    }
  });
}

void CFX_AggRenderer::CompositeSpanARGB(uint8_t* dest_scan,
                                        int bytes_per_pixel,
                                        int col_start,
                                        int col_end,
                                        const uint8_t* cover_scan,
                                        const uint8_t* clip_scan) {
  const auto& bgr = GetBGR();
  UNSAFE_TODO({
    dest_scan += col_start * bytes_per_pixel;
    if (rgb_byte_order_) {
      for (int col = col_start; col < col_end; col++) {
        int src_alpha = full_cover_
                            ? GetSrcAlpha(clip_scan, col)
                            : GetSourceAlpha(cover_scan, clip_scan, col);
        if (src_alpha) {
          if (src_alpha == 255) {
            *(reinterpret_cast<uint32_t*>(dest_scan)) = color_;
          } else {
            uint8_t dest_alpha =
                dest_scan[3] + src_alpha - dest_scan[3] * src_alpha / 255;
            dest_scan[3] = dest_alpha;
            int alpha_ratio = src_alpha * 255 / dest_alpha;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.red, alpha_ratio);
            dest_scan++;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.green, alpha_ratio);
            dest_scan++;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.blue, alpha_ratio);
            dest_scan += 2;
            continue;
          }
        }
        dest_scan += 4;
      }
      return;
    }
    for (int col = col_start; col < col_end; col++) {
      int src_alpha = full_cover_ ? GetSrcAlpha(clip_scan, col)
                                  : GetSourceAlpha(cover_scan, clip_scan, col);
      if (src_alpha) {
        if (src_alpha == 255) {
          *(reinterpret_cast<uint32_t*>(dest_scan)) = color_;
        } else {
          if (dest_scan[3] == 0) {
            dest_scan[3] = src_alpha;
            *dest_scan++ = bgr.blue;
            *dest_scan++ = bgr.green;
            *dest_scan = bgr.red;
            dest_scan += 2;
            continue;
          }
          uint8_t dest_alpha =
              dest_scan[3] + src_alpha - dest_scan[3] * src_alpha / 255;
          dest_scan[3] = dest_alpha;
          int alpha_ratio = src_alpha * 255 / dest_alpha;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.blue, alpha_ratio);
          dest_scan++;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.green, alpha_ratio);
          dest_scan++;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.red, alpha_ratio);
          dest_scan += 2;
          continue;
        }
      }
      dest_scan += bytes_per_pixel;
    }
  });
}

void CFX_AggRenderer::CompositeSpanRGB(uint8_t* dest_scan,
                                       int bytes_per_pixel,
                                       int col_start,
                                       int col_end,
                                       const uint8_t* cover_scan,
                                       const uint8_t* clip_scan) {
  const auto& bgr = GetBGR();
  UNSAFE_TODO({
    dest_scan += col_start * bytes_per_pixel;
    if (rgb_byte_order_) {
      for (int col = col_start; col < col_end; col++) {
        int src_alpha = GetSourceAlpha(cover_scan, clip_scan, col);
        if (src_alpha) {
          if (src_alpha == 255) {
            if (bytes_per_pixel == 4) {
              *(uint32_t*)dest_scan = color_;
            } else if (bytes_per_pixel == 3) {
              *dest_scan++ = bgr.red;
              *dest_scan++ = bgr.green;
              *dest_scan++ = bgr.blue;
              continue;
            }
          } else {
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.red, src_alpha);
            dest_scan++;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.green, src_alpha);
            dest_scan++;
            *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.blue, src_alpha);
            dest_scan += bytes_per_pixel - 2;
            continue;
          }
        }
        dest_scan += bytes_per_pixel;
      }
      return;
    }
    for (int col = col_start; col < col_end; col++) {
      int src_alpha = full_cover_ ? GetSrcAlpha(clip_scan, col)
                                  : GetSourceAlpha(cover_scan, clip_scan, col);
      if (src_alpha) {
        if (src_alpha == 255) {
          if (bytes_per_pixel == 4) {
            *(uint32_t*)dest_scan = color_;
          } else if (bytes_per_pixel == 3) {
            *dest_scan++ = bgr.blue;
            *dest_scan++ = bgr.green;
            *dest_scan++ = bgr.red;
            continue;
          }
        } else {
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.blue, src_alpha);
          dest_scan++;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.green, src_alpha);
          dest_scan++;
          *dest_scan = FXDIB_ALPHA_MERGE(*dest_scan, bgr.red, src_alpha);
          dest_scan += bytes_per_pixel - 2;
          continue;
        }
      }
      dest_scan += bytes_per_pixel;
    }
  });
}

CFX_AggRenderer::CFX_AggRenderer(const RetainPtr<CFX_DIBitmap>& pDevice,
                                 const RetainPtr<CFX_DIBitmap>& pBackdropDevice,
                                 const CFX_AggClipRgn* pClipRgn,
                                 uint32_t color,
                                 bool bFullCover,
                                 bool bRgbByteOrder)
    : alpha_(FXARGB_A(color)),
      color_(bRgbByteOrder ? FXARGB_TOBGRORDERDIB(color) : color),
      full_cover_(bFullCover),
      rgb_byte_order_(bRgbByteOrder),
      clip_box_(GetClipBoxFromRegion(pDevice, pClipRgn)),
      backdrop_device_(pBackdropDevice),
      clip_mask_(GetClipMaskFromRegion(pClipRgn)),
      device_(pDevice),
      clip_rgn_(pClipRgn),
      composite_span_func_(GetCompositeSpanFunc(device_)) {
  if (device_->GetBPP() == 8) {
    DCHECK(!rgb_byte_order_);
    if (device_->IsMaskFormat()) {
      color_data_ = 255;
    } else {
      color_data_ =
          FXRGB2GRAY(FXARGB_R(color), FXARGB_G(color), FXARGB_B(color));
    }
    return;
  }

  color_data_ = ArgbToBGRStruct(color);
}

template <class Scanline>
void CFX_AggRenderer::render(const Scanline& sl) {
  int y = sl.y();
  if (y < clip_box_.top || y >= clip_box_.bottom) {
    return;
  }

  uint8_t* dest_scan =
      device_->GetWritableBuffer().subspan(device_->GetPitch() * y).data();
  const uint8_t* backdrop_scan = nullptr;
  if (backdrop_device_) {
    backdrop_scan = backdrop_device_->GetBuffer()
                        .subspan(backdrop_device_->GetPitch() * y)
                        .data();
  }
  const int bytes_per_pixel = device_->GetBPP() / 8;
  CHECK_NE(bytes_per_pixel, 0);
  bool bDestAlpha = device_->IsAlphaFormat() || device_->IsMaskFormat();
  unsigned num_spans = sl.num_spans();
  typename Scanline::const_iterator span = sl.begin();
  UNSAFE_TODO({
    while (true) {
      if (span->len <= 0) {
        break;
      }

      int x = span->x;
      uint8_t* dest_pos = dest_scan + x * bytes_per_pixel;
      const uint8_t* backdrop_pos =
          backdrop_scan ? backdrop_scan + x * bytes_per_pixel : nullptr;
      const uint8_t* clip_pos = nullptr;
      if (clip_mask_) {
        // TODO(crbug.com/1382604): use subspan arithmetic.
        clip_pos = clip_mask_->GetBuffer().data() +
                   (y - clip_box_.top) * clip_mask_->GetPitch() + x -
                   clip_box_.left;
      }
      const int col_start = GetColStart(x, clip_box_.left);
      const int col_end = GetColEnd(x, span->len, clip_box_.right);
      if (backdrop_pos) {
        CompositeSpan(dest_pos, backdrop_pos, bytes_per_pixel, bDestAlpha,
                      col_start, col_end, span->covers, clip_pos);
      } else {
        (this->*composite_span_func_)(dest_pos, bytes_per_pixel, col_start,
                                      col_end, span->covers, clip_pos);
      }
      if (--num_spans == 0) {
        break;
      }

      ++span;
    }
  });
}

template <class BaseRenderer>
class RendererScanLineAaOffset {
 public:
  typedef BaseRenderer base_ren_type;
  typedef typename base_ren_type::color_type color_type;
  RendererScanLineAaOffset(base_ren_type& ren, unsigned left, unsigned top)
      : ren_(&ren), left_(left), top_(top) {}
  void color(const color_type& c) { color_ = c; }
  const color_type& color() const { return color_; }
  void prepare(unsigned) {}
  template <class Scanline>
  void render(const Scanline& sl) {
    int y = sl.y();
    unsigned num_spans = sl.num_spans();
    typename Scanline::const_iterator span = sl.begin();
    while (true) {
      int x = span->x;
      if (span->len > 0) {
        ren_->blend_solid_hspan(x - left_, y - top_, (unsigned)span->len,
                                color_, span->covers);
      } else {
        ren_->blend_hline(x - left_, y - top_, (unsigned)(x - span->len - 1),
                          color_, *(span->covers));
      }
      if (--num_spans == 0) {
        break;
      }

      UNSAFE_TODO(++span);
    }
  }

 private:
  UNOWNED_PTR_EXCLUSION base_ren_type* ren_;
  color_type color_;
  unsigned left_;
  unsigned top_;
};

agg::path_storage BuildAggPath(const CFX_Path& path,
                               const CFX_Matrix* pObject2Device) {
  agg::path_storage agg_path;
  pdfium::span<const CFX_Path::Point> points = path.GetPoints();
  for (size_t i = 0; i < points.size(); ++i) {
    CFX_PointF pos = points[i].point_;
    if (pObject2Device) {
      pos = pObject2Device->Transform(pos);
    }

    pos = HardClip(pos);
    CFX_Path::Point::Type point_type = points[i].type_;
    if (point_type == CFX_Path::Point::Type::kMove) {
      agg_path.move_to(pos.x, pos.y);
    } else if (point_type == CFX_Path::Point::Type::kLine) {
      if (i > 0 && points[i - 1].IsTypeAndOpen(CFX_Path::Point::Type::kMove) &&
          (i + 1 == points.size() ||
           points[i + 1].IsTypeAndOpen(CFX_Path::Point::Type::kMove)) &&
          points[i].point_ == points[i - 1].point_) {
        pos.x += 1;
      }
      agg_path.line_to(pos.x, pos.y);
    } else if (point_type == CFX_Path::Point::Type::kBezier) {
      if (i > 0 && i + 2 < points.size()) {
        CFX_PointF pos0 = points[i - 1].point_;
        CFX_PointF pos2 = points[i + 1].point_;
        CFX_PointF pos3 = points[i + 2].point_;
        if (pObject2Device) {
          pos0 = pObject2Device->Transform(pos0);
          pos2 = pObject2Device->Transform(pos2);
          pos3 = pObject2Device->Transform(pos3);
        }
        pos0 = HardClip(pos0);
        pos2 = HardClip(pos2);
        pos3 = HardClip(pos3);
        agg::curve4 curve(pos0.x, pos0.y, pos.x, pos.y, pos2.x, pos2.y, pos3.x,
                          pos3.y);
        i += 2;
        agg_path.add_path(curve);
      }
    }
    if (points[i].close_figure_) {
      agg_path.end_poly();
    }
  }
  return agg_path;
}

}  // namespace

CFX_AggDeviceDriver::CFX_AggDeviceDriver(
    RetainPtr<CFX_DIBitmap> pBitmap,
    bool bRgbByteOrder,
    RetainPtr<CFX_DIBitmap> pBackdropBitmap,
    bool bGroupKnockout)
    : bitmap_(std::move(pBitmap)),
      rgb_byte_order_(bRgbByteOrder),
      group_knockout_(bGroupKnockout),
      backdrop_bitmap_(std::move(pBackdropBitmap)) {
  CHECK(bitmap_);
  CHECK_NE(bitmap_->GetFormat(), FXDIB_Format::k1bppMask);
  CHECK_NE(bitmap_->GetFormat(), FXDIB_Format::k1bppRgb);
  InitPlatform();
}

CFX_AggDeviceDriver::~CFX_AggDeviceDriver() {
  DestroyPlatform();
}

#if !BUILDFLAG(IS_APPLE)
void CFX_AggDeviceDriver::InitPlatform() {}

void CFX_AggDeviceDriver::DestroyPlatform() {}

bool CFX_AggDeviceDriver::DrawDeviceText(
    pdfium::span<const TextCharPos> pCharPos,
    CFX_Font* pFont,
    const CFX_Matrix& mtObject2Device,
    float font_size,
    uint32_t color,
    const CFX_TextRenderOptions& options) {
  return false;
}
#endif  // !BUILDFLAG(IS_APPLE)

DeviceType CFX_AggDeviceDriver::GetDeviceType() const {
  return DeviceType::kDisplay;
}

int CFX_AggDeviceDriver::GetDeviceCaps(int caps_id) const {
  switch (caps_id) {
    case FXDC_PIXEL_WIDTH:
      return bitmap_->GetWidth();
    case FXDC_PIXEL_HEIGHT:
      return bitmap_->GetHeight();
    case FXDC_BITS_PIXEL:
      return bitmap_->GetBPP();
    case FXDC_HORZ_SIZE:
    case FXDC_VERT_SIZE:
      return 0;
    case FXDC_RENDER_CAPS: {
      int flags = FXRC_GET_BITS | FXRC_ALPHA_PATH | FXRC_ALPHA_IMAGE |
                  FXRC_BLEND_MODE | FXRC_SOFT_CLIP;
      if (bitmap_->IsAlphaFormat()) {
        flags |= FXRC_ALPHA_OUTPUT;
      } else if (bitmap_->IsMaskFormat()) {
        CHECK_NE(bitmap_->GetBPP(), 1);  // Matches format CHECKs in the ctor.
        flags |= FXRC_BYTEMASK_OUTPUT;
      }
      return flags;
    }
    default:
      NOTREACHED();
  }
}

void CFX_AggDeviceDriver::SaveState() {
  std::unique_ptr<CFX_AggClipRgn> pClip;
  if (clip_rgn_) {
    pClip = std::make_unique<CFX_AggClipRgn>(*clip_rgn_);
  }
  state_stack_.push_back(std::move(pClip));
}

void CFX_AggDeviceDriver::RestoreState(bool bKeepSaved) {
  clip_rgn_.reset();

  if (state_stack_.empty()) {
    return;
  }

  if (bKeepSaved) {
    if (state_stack_.back()) {
      clip_rgn_ = std::make_unique<CFX_AggClipRgn>(*state_stack_.back());
    }
  } else {
    clip_rgn_ = std::move(state_stack_.back());
    state_stack_.pop_back();
  }
}

void CFX_AggDeviceDriver::SetClipMask(agg::rasterizer_scanline_aa& rasterizer) {
  FX_RECT path_rect(rasterizer.min_x(), rasterizer.min_y(),
                    rasterizer.max_x() + 1, rasterizer.max_y() + 1);
  path_rect.Intersect(clip_rgn_->GetBox());
  auto pThisLayer = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!path_rect.IsEmpty()) {
    CHECK(pThisLayer->Create(path_rect.Width(), path_rect.Height(),
                             FXDIB_Format::k8bppMask));
    agg::rendering_buffer raw_buf(
        pThisLayer->GetWritableBuffer().data(), pThisLayer->GetWidth(),
        pThisLayer->GetHeight(), pThisLayer->GetPitch());
    agg::pixfmt_gray8 pixel_buf(raw_buf);
    agg::renderer_base<agg::pixfmt_gray8> base_buf(pixel_buf);
    RendererScanLineAaOffset<agg::renderer_base<agg::pixfmt_gray8>>
        final_render(base_buf, path_rect.left, path_rect.top);
    final_render.color(agg::gray8(255));
    agg::scanline_u8 scanline;
    agg::render_scanlines(rasterizer, scanline, final_render,
                          fill_options_.aliased_path);
  }
  clip_rgn_->IntersectMaskF(path_rect.left, path_rect.top,
                            std::move(pThisLayer));
}

bool CFX_AggDeviceDriver::SetClip_PathFill(
    const CFX_Path& path,
    const CFX_Matrix* pObject2Device,
    const CFX_FillRenderOptions& fill_options) {
  DCHECK(fill_options.fill_type != CFX_FillRenderOptions::FillType::kNoFill);

  fill_options_ = fill_options;
  if (!clip_rgn_) {
    clip_rgn_ = std::make_unique<CFX_AggClipRgn>(
        GetDeviceCaps(FXDC_PIXEL_WIDTH), GetDeviceCaps(FXDC_PIXEL_HEIGHT));
  }
  std::optional<CFX_FloatRect> maybe_rectf = path.GetRect(pObject2Device);
  if (maybe_rectf.has_value()) {
    CFX_FloatRect& rectf = maybe_rectf.value();
    rectf.Intersect(
        CFX_FloatRect(0, 0, static_cast<float>(GetDeviceCaps(FXDC_PIXEL_WIDTH)),
                      static_cast<float>(GetDeviceCaps(FXDC_PIXEL_HEIGHT))));
    FX_RECT rect = rectf.GetOuterRect();
    clip_rgn_->IntersectRect(rect);
    return true;
  }
  agg::path_storage path_data = BuildAggPath(path, pObject2Device);
  path_data.end_poly();
  agg::rasterizer_scanline_aa rasterizer;
  rasterizer.clip_box(0.0f, 0.0f,
                      static_cast<float>(GetDeviceCaps(FXDC_PIXEL_WIDTH)),
                      static_cast<float>(GetDeviceCaps(FXDC_PIXEL_HEIGHT)));
  rasterizer.add_path(path_data);
  rasterizer.filling_rule(GetAlternateOrWindingFillType(fill_options));
  SetClipMask(rasterizer);
  return true;
}

bool CFX_AggDeviceDriver::SetClip_PathStroke(
    const CFX_Path& path,
    const CFX_Matrix* pObject2Device,
    const CFX_GraphStateData* pGraphState) {
  if (!clip_rgn_) {
    clip_rgn_ = std::make_unique<CFX_AggClipRgn>(
        GetDeviceCaps(FXDC_PIXEL_WIDTH), GetDeviceCaps(FXDC_PIXEL_HEIGHT));
  }
  agg::path_storage path_data = BuildAggPath(path, nullptr);
  agg::rasterizer_scanline_aa rasterizer;
  rasterizer.clip_box(0.0f, 0.0f,
                      static_cast<float>(GetDeviceCaps(FXDC_PIXEL_WIDTH)),
                      static_cast<float>(GetDeviceCaps(FXDC_PIXEL_HEIGHT)));
  RasterizeStroke(&rasterizer, &path_data, pObject2Device, pGraphState, 1.0f,
                  false);
  rasterizer.filling_rule(agg::fill_non_zero);
  SetClipMask(rasterizer);
  return true;
}

int CFX_AggDeviceDriver::GetDriverType() const {
  return 1;
}

bool CFX_AggDeviceDriver::MultiplyAlpha(float alpha) {
  return bitmap_->MultiplyAlpha(alpha);
}

bool CFX_AggDeviceDriver::MultiplyAlphaMask(
    RetainPtr<const CFX_DIBitmap> mask) {
  return bitmap_->MultiplyAlphaMask(std::move(mask));
}

void CFX_AggDeviceDriver::Clear(uint32_t color) {
  bitmap_->Clear(color);
}

void CFX_AggDeviceDriver::RenderRasterizer(
    agg::rasterizer_scanline_aa& rasterizer,
    uint32_t color,
    bool bFullCover,
    bool bGroupKnockout) {
  RetainPtr<CFX_DIBitmap> pt = bGroupKnockout ? backdrop_bitmap_ : nullptr;
  CFX_AggRenderer render(bitmap_, pt, clip_rgn_.get(), color, bFullCover,
                         rgb_byte_order_);
  agg::scanline_u8 scanline;
  agg::render_scanlines(rasterizer, scanline, render,
                        fill_options_.aliased_path);
}

bool CFX_AggDeviceDriver::DrawPath(const CFX_Path& path,
                                   const CFX_Matrix* pObject2Device,
                                   const CFX_GraphStateData* pGraphState,
                                   uint32_t fill_color,
                                   uint32_t stroke_color,
                                   const CFX_FillRenderOptions& fill_options) {
  if (bitmap_->GetBuffer().empty()) {
    return true;
  }

  fill_options_ = fill_options;
  if (fill_options.fill_type != CFX_FillRenderOptions::FillType::kNoFill &&
      fill_color) {
    agg::path_storage path_data = BuildAggPath(path, pObject2Device);
    agg::rasterizer_scanline_aa rasterizer;
    rasterizer.clip_box(0.0f, 0.0f,
                        static_cast<float>(GetDeviceCaps(FXDC_PIXEL_WIDTH)),
                        static_cast<float>(GetDeviceCaps(FXDC_PIXEL_HEIGHT)));
    rasterizer.add_path(path_data);
    rasterizer.filling_rule(GetAlternateOrWindingFillType(fill_options));
    RenderRasterizer(rasterizer, fill_color, fill_options.full_cover,
                     /*bGroupKnockout=*/false);
  }
  int stroke_alpha = FXARGB_A(stroke_color);
  if (!pGraphState || !stroke_alpha) {
    return true;
  }

  if (fill_options.zero_area) {
    agg::path_storage path_data = BuildAggPath(path, pObject2Device);
    agg::rasterizer_scanline_aa rasterizer;
    rasterizer.clip_box(0.0f, 0.0f,
                        static_cast<float>(GetDeviceCaps(FXDC_PIXEL_WIDTH)),
                        static_cast<float>(GetDeviceCaps(FXDC_PIXEL_HEIGHT)));
    RasterizeStroke(&rasterizer, &path_data, nullptr, pGraphState, 1,
                    fill_options.stroke_text_mode);
    RenderRasterizer(rasterizer, stroke_color, fill_options.full_cover,
                     group_knockout_);
    return true;
  }
  CFX_Matrix matrix1;
  CFX_Matrix matrix2;
  if (pObject2Device) {
    matrix1.a = std::max(fabs(pObject2Device->a), fabs(pObject2Device->b));
    matrix1.d = matrix1.a;
    matrix2 = CFX_Matrix(
        pObject2Device->a / matrix1.a, pObject2Device->b / matrix1.a,
        pObject2Device->c / matrix1.d, pObject2Device->d / matrix1.d, 0, 0);

    matrix1 = *pObject2Device * matrix2.GetInverse();
  }

  agg::path_storage path_data = BuildAggPath(path, &matrix1);
  agg::rasterizer_scanline_aa rasterizer;
  rasterizer.clip_box(0.0f, 0.0f,
                      static_cast<float>(GetDeviceCaps(FXDC_PIXEL_WIDTH)),
                      static_cast<float>(GetDeviceCaps(FXDC_PIXEL_HEIGHT)));
  RasterizeStroke(&rasterizer, &path_data, &matrix2, pGraphState, matrix1.a,
                  fill_options.stroke_text_mode);
  RenderRasterizer(rasterizer, stroke_color, fill_options.full_cover,
                   group_knockout_);
  return true;
}

bool CFX_AggDeviceDriver::FillRect(const FX_RECT& rect, uint32_t fill_color) {
  if (bitmap_->GetBuffer().empty()) {
    return true;
  }

  FX_RECT clip_rect = GetClipBox();
  FX_RECT draw_rect = clip_rect;
  draw_rect.Intersect(rect);
  if (draw_rect.IsEmpty()) {
    return true;
  }

  if (!clip_rgn_ || clip_rgn_->GetType() == CFX_AggClipRgn::kRectI) {
    if (rgb_byte_order_) {
      RgbByteOrderCompositeRect(bitmap_, draw_rect.left, draw_rect.top,
                                draw_rect.Width(), draw_rect.Height(),
                                fill_color);
    } else {
      bitmap_->CompositeRect(draw_rect.left, draw_rect.top, draw_rect.Width(),
                             draw_rect.Height(), fill_color);
    }
    return true;
  }
  bitmap_->CompositeMask(draw_rect.left, draw_rect.top, draw_rect.Width(),
                         draw_rect.Height(), clip_rgn_->GetMask(), fill_color,
                         draw_rect.left - clip_rect.left,
                         draw_rect.top - clip_rect.top, BlendMode::kNormal,
                         nullptr, rgb_byte_order_);
  return true;
}

FX_RECT CFX_AggDeviceDriver::GetClipBox() const {
  if (clip_rgn_) {
    return clip_rgn_->GetBox();
  }
  return FX_RECT(0, 0, GetDeviceCaps(FXDC_PIXEL_WIDTH),
                 GetDeviceCaps(FXDC_PIXEL_HEIGHT));
}

bool CFX_AggDeviceDriver::GetDIBits(RetainPtr<CFX_DIBitmap> bitmap,
                                    int left,
                                    int top) const {
  if (bitmap_->GetBuffer().empty()) {
    return true;
  }

  FX_RECT rect(left, top, left + bitmap->GetWidth(), top + bitmap->GetHeight());
  RetainPtr<CFX_DIBitmap> pBack;
  if (backdrop_bitmap_) {
    pBack = backdrop_bitmap_->ClipTo(rect);
    if (!pBack) {
      return true;
    }

    pBack->CompositeBitmap(0, 0, pBack->GetWidth(), pBack->GetHeight(), bitmap_,
                           0, 0, BlendMode::kNormal, nullptr, false);
  } else {
    pBack = bitmap_->ClipTo(rect);
    if (!pBack) {
      return true;
    }
  }

  left = std::min(left, 0);
  top = std::min(top, 0);
  if (rgb_byte_order_) {
    RgbByteOrderTransferBitmap(std::move(bitmap), rect.Width(), rect.Height(),
                               std::move(pBack), left, top);
    return true;
  }
  return bitmap->TransferBitmap(rect.Width(), rect.Height(), std::move(pBack),
                                left, top);
}

RetainPtr<const CFX_DIBitmap> CFX_AggDeviceDriver::GetBackDrop() const {
  return backdrop_bitmap_;
}

bool CFX_AggDeviceDriver::SetDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                                    uint32_t argb,
                                    const FX_RECT& src_rect,
                                    int left,
                                    int top,
                                    BlendMode blend_type) {
  if (bitmap_->GetBuffer().empty()) {
    return true;
  }

  if (bitmap->IsMaskFormat()) {
    return bitmap_->CompositeMask(left, top, src_rect.Width(),
                                  src_rect.Height(), std::move(bitmap), argb,
                                  src_rect.left, src_rect.top, blend_type,
                                  clip_rgn_.get(), rgb_byte_order_);
  }
  return bitmap_->CompositeBitmap(left, top, src_rect.Width(),
                                  src_rect.Height(), std::move(bitmap),
                                  src_rect.left, src_rect.top, blend_type,
                                  clip_rgn_.get(), rgb_byte_order_);
}

bool CFX_AggDeviceDriver::StretchDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                                        uint32_t argb,
                                        int dest_left,
                                        int dest_top,
                                        int dest_width,
                                        int dest_height,
                                        const FX_RECT* pClipRect,
                                        const FXDIB_ResampleOptions& options,
                                        BlendMode blend_type) {
  if (bitmap_->GetBuffer().empty()) {
    return true;
  }

  if (dest_width == bitmap->GetWidth() && dest_height == bitmap->GetHeight()) {
    FX_RECT rect(0, 0, dest_width, dest_height);
    return SetDIBits(std::move(bitmap), argb, rect, dest_left, dest_top,
                     blend_type);
  }
  FX_RECT dest_rect(dest_left, dest_top, dest_left + dest_width,
                    dest_top + dest_height);
  dest_rect.Normalize();
  FX_RECT dest_clip = dest_rect;
  dest_clip.Intersect(*pClipRect);
  CFX_AggBitmapComposer composer;
  composer.Compose(bitmap_, clip_rgn_.get(), /*alpha=*/1.0f, argb, dest_clip,
                   /*bVertical=*/false, /*bFlipX=*/false, /*bFlipY=*/false,
                   rgb_byte_order_, blend_type);
  dest_clip.Offset(-dest_rect.left, -dest_rect.top);
  CFX_ImageStretcher stretcher(&composer, std::move(bitmap), dest_width,
                               dest_height, dest_clip, options);
  if (stretcher.Start()) {
    stretcher.Continue(nullptr);
  }
  return true;
}

RenderDeviceDriverIface::StartResult CFX_AggDeviceDriver::StartDIBits(
    RetainPtr<const CFX_DIBBase> bitmap,
    float alpha,
    uint32_t argb,
    const CFX_Matrix& matrix,
    const FXDIB_ResampleOptions& options,
    BlendMode blend_type) {
  if (bitmap_->GetBuffer().empty()) {
    return {Result::kSuccess, nullptr};
  }

  return {Result::kSuccess, std::make_unique<CFX_AggImageRenderer>(
                                bitmap_, clip_rgn_.get(), std::move(bitmap),
                                alpha, argb, matrix, options, rgb_byte_order_)};
}

bool CFX_AggDeviceDriver::ContinueDIBits(CFX_AggImageRenderer* pHandle,
                                         PauseIndicatorIface* pPause) {
  return bitmap_->GetBuffer().empty() || pHandle->Continue(pPause);
}

}  // namespace pdfium

bool CFX_DefaultRenderDevice::AttachAggImpl(
    RetainPtr<CFX_DIBitmap> pBitmap,
    bool bRgbByteOrder,
    RetainPtr<CFX_DIBitmap> pBackdropBitmap,
    bool bGroupKnockout) {
  // Unlike the Skia version, all callers pass in a non-null `pBitmap`.
  CHECK(pBitmap);
  SetBitmap(pBitmap);
  SetDeviceDriver(std::make_unique<pdfium::CFX_AggDeviceDriver>(
      std::move(pBitmap), bRgbByteOrder, std::move(pBackdropBitmap),
      bGroupKnockout));
  return true;
}

bool CFX_DefaultRenderDevice::CreateAgg(
    int width,
    int height,
    FXDIB_Format format,
    RetainPtr<CFX_DIBitmap> pBackdropBitmap) {
  auto pBitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!pBitmap->Create(width, height, format)) {
    return false;
  }

  SetBitmap(pBitmap);
  SetDeviceDriver(std::make_unique<pdfium::CFX_AggDeviceDriver>(
      std::move(pBitmap), false, std::move(pBackdropBitmap), false));
  return true;
}
