// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fxge/cfx_renderdevice.h"

#include <math.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include "build/build_config.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/zip.h"
#include "core/fxge/cfx_color.h"
#include "core/fxge/cfx_defaultrenderdevice.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_fontmgr.h"
#include "core/fxge/cfx_gemodule.h"
#include "core/fxge/cfx_glyphbitmap.h"
#include "core/fxge/cfx_glyphcache.h"
#include "core/fxge/cfx_graphstatedata.h"
#include "core/fxge/cfx_path.h"
#include "core/fxge/cfx_textrenderoptions.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "core/fxge/fx_font.h"
#include "core/fxge/renderdevicedriver_iface.h"
#include "core/fxge/text_char_pos.h"
#include "core/fxge/text_glyph_pos.h"

#if defined(PDF_USE_SKIA)
#include "third_party/skia/include/core/SkTypes.h"  // nogncheck
#endif

namespace {

void AdjustGlyphSpace(std::vector<TextGlyphPos>* pGlyphAndPos) {
  DCHECK_GT(pGlyphAndPos->size(), 1u);
  std::vector<TextGlyphPos>& glyphs = *pGlyphAndPos;
  bool bVertical = glyphs.back().origin_.x == glyphs.front().origin_.x;
  if (!bVertical && (glyphs.back().origin_.y != glyphs.front().origin_.y)) {
    return;
  }

  for (size_t i = glyphs.size() - 1; i > 1; --i) {
    const TextGlyphPos& next = glyphs[i];
    int next_origin = bVertical ? next.origin_.y : next.origin_.x;
    float next_origin_f =
        bVertical ? next.device_origin_.y : next.device_origin_.x;

    TextGlyphPos& current = glyphs[i - 1];
    int& current_origin = bVertical ? current.origin_.y : current.origin_.x;
    float current_origin_f =
        bVertical ? current.device_origin_.y : current.device_origin_.x;

    FX_SAFE_INT32 safe_space = next_origin;
    safe_space -= current_origin;
    if (!safe_space.IsValid()) {
      continue;
    }

    int space = safe_space.ValueOrDie();
    float space_f = next_origin_f - current_origin_f;
    float error = fabs(space_f) - fabs(static_cast<float>(space));
    if (error <= 0.5f) {
      continue;
    }

    FX_SAFE_INT32 safe_origin = current_origin;
    safe_origin += space > 0 ? -1 : 1;
    if (!safe_origin.IsValid()) {
      continue;
    }

    current_origin = safe_origin.ValueOrDie();
  }
}

constexpr std::array<const uint8_t, 256> kTextGammaAdjust = {{
    0,   2,   3,   4,   6,   7,   8,   10,  11,  12,  13,  15,  16,  17,  18,
    19,  21,  22,  23,  24,  25,  26,  27,  29,  30,  31,  32,  33,  34,  35,
    36,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  51,  52,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,
    68,  69,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,
    84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,
    99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113,
    114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128,
    129, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,
    143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 156,
    157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171,
    172, 173, 174, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185,
    186, 187, 188, 189, 190, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199,
    200, 201, 202, 203, 204, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213,
    214, 215, 216, 217, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227,
    228, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 239, 240,
    241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 250, 251, 252, 253, 254,
    255,
}};

int TextGammaAdjust(int value) {
  return kTextGammaAdjust[value];
}

int CalcAlpha(int src, int alpha) {
  return src * alpha / 255;
}

void MergeGammaAdjust(uint8_t src, int channel, int alpha, uint8_t* dest) {
  *dest =
      FXDIB_ALPHA_MERGE(*dest, channel, CalcAlpha(TextGammaAdjust(src), alpha));
}

void MergeGammaAdjustRgb(const uint8_t* src,
                         const FX_BGRA_STRUCT<uint8_t>& bgra,
                         uint8_t* dest) {
  UNSAFE_TODO({
    MergeGammaAdjust(src[2], bgra.blue, bgra.alpha, &dest[0]);
    MergeGammaAdjust(src[1], bgra.green, bgra.alpha, &dest[1]);
    MergeGammaAdjust(src[0], bgra.red, bgra.alpha, &dest[2]);
  });
}

int AverageRgb(const uint8_t* src) {
  return UNSAFE_TODO((src[0] + src[1] + src[2]) / 3);
}

uint8_t CalculateDestAlpha(uint8_t back_alpha, int src_alpha) {
  return back_alpha + src_alpha - back_alpha * src_alpha / 255;
}

void ApplyAlpha(uint8_t* dest, const FX_BGRA_STRUCT<uint8_t>& bgra, int alpha) {
  UNSAFE_TODO({
    dest[0] = FXDIB_ALPHA_MERGE(dest[0], bgra.blue, alpha);
    dest[1] = FXDIB_ALPHA_MERGE(dest[1], bgra.green, alpha);
    dest[2] = FXDIB_ALPHA_MERGE(dest[2], bgra.red, alpha);
  });
}

void ApplyDestAlpha(uint8_t back_alpha,
                    int src_alpha,
                    const FX_BGRA_STRUCT<uint8_t>& bgra,
                    uint8_t* dest) {
  uint8_t dest_alpha = CalculateDestAlpha(back_alpha, src_alpha);
  ApplyAlpha(dest, bgra, src_alpha * 255 / dest_alpha);
  UNSAFE_TODO(dest[3] = dest_alpha);
}

void NormalizeArgb(int src_value,
                   const FX_BGRA_STRUCT<uint8_t>& bgra,
                   uint8_t* dest,
                   int src_alpha) {
  UNSAFE_TODO({
    uint8_t back_alpha = dest[3];
    if (back_alpha == 0) {
      FXARGB_SetDIB(dest,
                    ArgbEncode(src_alpha, bgra.red, bgra.green, bgra.blue));
    } else if (src_alpha != 0) {
      ApplyDestAlpha(back_alpha, src_alpha, bgra, dest);
    }
  });
}

void NormalizeDest(bool has_alpha,
                   int src_value,
                   const FX_BGRA_STRUCT<uint8_t>& bgra,
                   uint8_t* dest) {
  if (has_alpha) {
    NormalizeArgb(src_value, bgra, dest,
                  CalcAlpha(TextGammaAdjust(src_value), bgra.alpha));
    return;
  }
  int src_alpha = CalcAlpha(TextGammaAdjust(src_value), bgra.alpha);
  if (src_alpha == 0) {
    return;
  }

  ApplyAlpha(dest, bgra, src_alpha);
}

void NormalizeSrc(bool has_alpha,
                  int src_value,
                  const FX_BGRA_STRUCT<uint8_t>& bgra,
                  uint8_t* dest) {
  if (!has_alpha) {
    ApplyAlpha(dest, bgra, CalcAlpha(TextGammaAdjust(src_value), bgra.alpha));
    return;
  }
  int src_alpha = CalcAlpha(TextGammaAdjust(src_value), bgra.alpha);
  if (src_alpha != 0) {
    NormalizeArgb(src_value, bgra, dest, src_alpha);
  }
}

void NextPixel(const uint8_t** src_scan, uint8_t** dst_scan, int bpp) {
  UNSAFE_TODO({
    *src_scan += 3;
    *dst_scan += bpp;
  });
}

void SetAlpha(bool has_alpha, uint8_t* alpha) {
  if (has_alpha) {
    UNSAFE_TODO(alpha[3] = 255);
  }
}

void DrawNormalTextHelper(const RetainPtr<CFX_DIBitmap>& bitmap,
                          const RetainPtr<CFX_DIBitmap>& pGlyph,
                          int nrows,
                          int left,
                          int top,
                          int start_col,
                          int end_col,
                          bool normalize,
                          int x_subpixel,
                          const FX_BGRA_STRUCT<uint8_t>& bgra) {
  // TODO(crbug.com/42271020): Add support for `FXDIB_Format::kBgraPremul`.
  CHECK(!bitmap->IsPremultiplied());
  const bool has_alpha = bitmap->IsAlphaFormat();
  const int bytes_per_pixel = has_alpha ? 4 : bitmap->GetBPP() / 8;
  for (int row = 0; row < nrows; ++row) {
    FX_SAFE_INT32 safe_dest_row = row;
    safe_dest_row += top;
    const int dest_row = safe_dest_row.ValueOrDefault(-1);
    if (dest_row < 0 || dest_row >= bitmap->GetHeight()) {
      continue;
    }

    const uint8_t* src_scan =
        pGlyph->GetScanline(row)
            .subspan(static_cast<size_t>((start_col - left) * 3))
            .data();
    uint8_t* dest_scan =
        bitmap->GetWritableScanline(dest_row)
            .subspan(static_cast<size_t>(start_col * bytes_per_pixel))
            .data();
    if (x_subpixel == 0) {
      for (int col = start_col; col < end_col; ++col) {
        if (normalize) {
          int src_value = AverageRgb(&src_scan[0]);
          NormalizeDest(has_alpha, src_value, bgra, dest_scan);
        } else {
          MergeGammaAdjustRgb(&src_scan[0], bgra, &dest_scan[0]);
          SetAlpha(has_alpha, dest_scan);
        }
        NextPixel(&src_scan, &dest_scan, bytes_per_pixel);
      }
      continue;
    }
    UNSAFE_TODO({
      if (x_subpixel == 1) {
        if (normalize) {
          int src_value = start_col > left ? AverageRgb(&src_scan[-1])
                                           : (src_scan[0] + src_scan[1]) / 3;
          NormalizeSrc(has_alpha, src_value, bgra, dest_scan);
        } else {
          if (start_col > left) {
            MergeGammaAdjust(src_scan[-1], bgra.red, bgra.alpha, &dest_scan[2]);
          }
          MergeGammaAdjust(src_scan[0], bgra.green, bgra.alpha, &dest_scan[1]);
          MergeGammaAdjust(src_scan[1], bgra.blue, bgra.alpha, &dest_scan[0]);
          SetAlpha(has_alpha, dest_scan);
        }
        NextPixel(&src_scan, &dest_scan, bytes_per_pixel);
        for (int col = start_col + 1; col < end_col; ++col) {
          if (normalize) {
            int src_value = AverageRgb(&src_scan[-1]);
            NormalizeDest(has_alpha, src_value, bgra, dest_scan);
          } else {
            MergeGammaAdjustRgb(&src_scan[-1], bgra, &dest_scan[0]);
            SetAlpha(has_alpha, dest_scan);
          }
          NextPixel(&src_scan, &dest_scan, bytes_per_pixel);
        }
        continue;
      }
      if (normalize) {
        int src_value =
            start_col > left ? AverageRgb(&src_scan[-2]) : src_scan[0] / 3;
        NormalizeSrc(has_alpha, src_value, bgra, dest_scan);
      } else {
        if (start_col > left) {
          MergeGammaAdjust(src_scan[-2], bgra.red, bgra.alpha, &dest_scan[2]);
          MergeGammaAdjust(src_scan[-1], bgra.green, bgra.alpha, &dest_scan[1]);
        }
        MergeGammaAdjust(src_scan[0], bgra.blue, bgra.alpha, &dest_scan[0]);
        SetAlpha(has_alpha, dest_scan);
      }
      NextPixel(&src_scan, &dest_scan, bytes_per_pixel);
      for (int col = start_col + 1; col < end_col; ++col) {
        if (normalize) {
          int src_value = AverageRgb(&src_scan[-2]);
          NormalizeDest(has_alpha, src_value, bgra, dest_scan);
        } else {
          MergeGammaAdjustRgb(&src_scan[-2], bgra, &dest_scan[0]);
          SetAlpha(has_alpha, dest_scan);
        }
        NextPixel(&src_scan, &dest_scan, bytes_per_pixel);
      }
    });
  }
}

bool ShouldDrawDeviceText(const CFX_Font* font,
                          const CFX_TextRenderOptions& options) {
#if BUILDFLAG(IS_APPLE)
  if (options.font_is_cid) {
    return false;
  }

  const ByteString bsPsName = font->GetPsName();
  if (bsPsName.Contains("+ZJHL")) {
    return false;
  }

  if (bsPsName == "CNAAJI+cmex10") {
    return false;
  }
#endif
  return true;
}

// Returns true if the path is a 3-point path that draws A->B->A and forms a
// zero area, or a 2-point path which draws A->B.
bool CheckSimpleLinePath(pdfium::span<const CFX_Path::Point> points,
                         const CFX_Matrix* matrix,
                         bool adjust,
                         CFX_Path* new_path,
                         bool* thin,
                         bool* set_identity) {
  if (points.size() != 2 && points.size() != 3) {
    return false;
  }

  if (points[0].type_ != CFX_Path::Point::Type::kMove ||
      points[1].type_ != CFX_Path::Point::Type::kLine ||
      (points.size() == 3 && (points[2].type_ != CFX_Path::Point::Type::kLine ||
                              points[0].point_ != points[2].point_))) {
    return false;
  }

  // A special case that all points are identical, zero area is formed and no
  // thin line needs to be drawn.
  if (points[0].point_ == points[1].point_) {
    return true;
  }

  for (size_t i = 0; i < 2; i++) {
    CFX_PointF point = points[i].point_;
    if (adjust) {
      if (matrix) {
        point = matrix->Transform(point);
      }

      point = CFX_PointF(static_cast<int>(point.x) + 0.5f,
                         static_cast<int>(point.y) + 0.5f);
    }
    new_path->AppendPoint(point, points[i].type_);
  }
  if (adjust && matrix) {
    *set_identity = true;
  }

  *thin = true;
  return true;
}

// Returns true if `points` is palindromic and forms zero area. Otherwise,
// returns false.
bool CheckPalindromicPath(pdfium::span<const CFX_Path::Point> points,
                          CFX_Path* new_path,
                          bool* thin) {
  if (points.size() <= 3 || !(points.size() % 2)) {
    return false;
  }

  const size_t mid = points.size() / 2;
  CFX_Path temp_path;
  for (size_t i = 0; i < mid; i++) {
    const CFX_Path::Point& left = points[mid - i - 1];
    const CFX_Path::Point& right = points[mid + i + 1];
    bool zero_area = left.point_ == right.point_ &&
                     left.type_ != CFX_Path::Point::Type::kBezier &&
                     right.type_ != CFX_Path::Point::Type::kBezier;
    if (!zero_area) {
      return false;
    }

    temp_path.AppendPoint(points[mid - i].point_, CFX_Path::Point::Type::kMove);
    temp_path.AppendPoint(left.point_, CFX_Path::Point::Type::kLine);
  }

  new_path->Append(temp_path, nullptr);
  *thin = true;
  return true;
}

bool IsFoldingVerticalLine(const CFX_PointF& a,
                           const CFX_PointF& b,
                           const CFX_PointF& c) {
  return a.x == b.x && b.x == c.x && (b.y - a.y) * (b.y - c.y) > 0;
}

bool IsFoldingHorizontalLine(const CFX_PointF& a,
                             const CFX_PointF& b,
                             const CFX_PointF& c) {
  return a.y == b.y && b.y == c.y && (b.x - a.x) * (b.x - c.x) > 0;
}

bool IsFoldingDiagonalLine(const CFX_PointF& a,
                           const CFX_PointF& b,
                           const CFX_PointF& c) {
  return a.x != b.x && c.x != b.x && a.y != b.y && c.y != b.y &&
         (a.y - b.y) * (c.x - b.x) == (c.y - b.y) * (a.x - b.x);
}

bool GetZeroAreaPath(pdfium::span<const CFX_Path::Point> points,
                     const CFX_Matrix* matrix,
                     bool adjust,
                     CFX_Path* new_path,
                     bool* thin,
                     bool* set_identity) {
  *set_identity = false;

  if (points.size() < 2) {
    return false;
  }

  if (CheckSimpleLinePath(points, matrix, adjust, new_path, thin,
                          set_identity)) {
    return true;
  }

  if (CheckPalindromicPath(points, new_path, thin)) {
    return true;
  }

  for (size_t i = 0; i < points.size(); i++) {
    CFX_Path::Point::Type point_type = points[i].type_;
    if (point_type == CFX_Path::Point::Type::kMove) {
      DCHECK_EQ(0u, i);
      continue;
    }

    if (point_type == CFX_Path::Point::Type::kBezier) {
      i += 2;
      DCHECK_LT(i, points.size());
      continue;
    }

    DCHECK_EQ(point_type, CFX_Path::Point::Type::kLine);
    size_t next_index = (i + 1) % (points.size());
    const CFX_Path::Point& next = points[next_index];
    if (next.type_ != CFX_Path::Point::Type::kLine) {
      continue;
    }

    const CFX_Path::Point& prev = points[i - 1];
    const CFX_Path::Point& cur = points[i];
    if (IsFoldingVerticalLine(prev.point_, cur.point_, next.point_)) {
      bool use_prev = fabs(cur.point_.y - prev.point_.y) <
                      fabs(cur.point_.y - next.point_.y);
      const CFX_Path::Point& start = use_prev ? prev : cur;
      const CFX_Path::Point& end = use_prev ? cur : next;
      new_path->AppendPoint(start.point_, CFX_Path::Point::Type::kMove);
      new_path->AppendPoint(end.point_, CFX_Path::Point::Type::kLine);
      continue;
    }

    if (IsFoldingHorizontalLine(prev.point_, cur.point_, next.point_) ||
        IsFoldingDiagonalLine(prev.point_, cur.point_, next.point_)) {
      bool use_prev = fabs(cur.point_.x - prev.point_.x) <
                      fabs(cur.point_.x - next.point_.x);
      const CFX_Path::Point& start = use_prev ? prev : cur;
      const CFX_Path::Point& end = use_prev ? cur : next;
      new_path->AppendPoint(start.point_, CFX_Path::Point::Type::kMove);
      new_path->AppendPoint(end.point_, CFX_Path::Point::Type::kLine);
      continue;
    }
  }

  size_t new_path_size = new_path->GetPoints().size();
  if (points.size() > 3 && new_path_size > 0) {
    *thin = true;
  }
  return new_path_size != 0;
}

FXDIB_Format GetCreateCompatibleBitmapFormat(int render_caps,
                                             bool use_argb_premul) {
  if (render_caps & FXRC_BYTEMASK_OUTPUT) {
    return FXDIB_Format::k8bppMask;
  }
#if defined(PDF_USE_SKIA)
  if (use_argb_premul && (render_caps & FXRC_PREMULTIPLIED_ALPHA)) {
    return FXDIB_Format::kBgraPremul;
  }
#endif
  if (render_caps & FXRC_ALPHA_OUTPUT) {
    return FXDIB_Format::kBgra;
  }
  return CFX_DIBBase::kPlatformRGBFormat;
}

}  // namespace

CFX_RenderDevice::CFX_RenderDevice() = default;

CFX_RenderDevice::~CFX_RenderDevice() {
  RestoreState(false);
}

// static
CFX_Matrix CFX_RenderDevice::GetFlipMatrix(float width,
                                           float height,
                                           float left,
                                           float top) {
  return CFX_Matrix(width, 0, 0, -height, left, top + height);
}

void CFX_RenderDevice::SetDeviceDriver(
    std::unique_ptr<RenderDeviceDriverIface> pDriver) {
  DCHECK(pDriver);
  DCHECK(!device_driver_);
  device_driver_ = std::move(pDriver);
  InitDeviceInfo();
}

void CFX_RenderDevice::InitDeviceInfo() {
  width_ = device_driver_->GetDeviceCaps(FXDC_PIXEL_WIDTH);
  height_ = device_driver_->GetDeviceCaps(FXDC_PIXEL_HEIGHT);
  bpp_ = device_driver_->GetDeviceCaps(FXDC_BITS_PIXEL);
  render_caps_ = device_driver_->GetDeviceCaps(FXDC_RENDER_CAPS);
  device_type_ = device_driver_->GetDeviceType();
  clip_box_ = device_driver_->GetClipBox();
}

void CFX_RenderDevice::SaveState() {
  device_driver_->SaveState();
}

void CFX_RenderDevice::RestoreState(bool bKeepSaved) {
  if (device_driver_) {
    device_driver_->RestoreState(bKeepSaved);
    UpdateClipBox();
  }
}

int CFX_RenderDevice::GetDeviceCaps(int caps_id) const {
  return device_driver_->GetDeviceCaps(caps_id);
}

RetainPtr<CFX_DIBitmap> CFX_RenderDevice::GetBitmap() {
  return bitmap_;
}

RetainPtr<const CFX_DIBitmap> CFX_RenderDevice::GetBitmap() const {
  return bitmap_;
}

void CFX_RenderDevice::SetBitmap(RetainPtr<CFX_DIBitmap> bitmap) {
  bitmap_ = std::move(bitmap);
}

bool CFX_RenderDevice::CreateCompatibleBitmap(
    const RetainPtr<CFX_DIBitmap>& pDIB,
    int width,
    int height) const {
  return pDIB->Create(
      width, height,
      GetCreateCompatibleBitmapFormat(render_caps_, /*use_argb_premul=*/true));
}

void CFX_RenderDevice::SetBaseClip(const FX_RECT& rect) {
  device_driver_->SetBaseClip(rect);
}

bool CFX_RenderDevice::SetClip_PathFill(
    const CFX_Path& path,
    const CFX_Matrix* pObject2Device,
    const CFX_FillRenderOptions& fill_options) {
  if (!device_driver_->SetClip_PathFill(path, pObject2Device, fill_options)) {
    return false;
  }

  UpdateClipBox();
  return true;
}

bool CFX_RenderDevice::SetClip_PathStroke(
    const CFX_Path& path,
    const CFX_Matrix* pObject2Device,
    const CFX_GraphStateData* pGraphState) {
  if (!device_driver_->SetClip_PathStroke(path, pObject2Device, pGraphState)) {
    return false;
  }

  UpdateClipBox();
  return true;
}

bool CFX_RenderDevice::SetClip_Rect(const FX_RECT& rect) {
  CFX_Path path;
  path.AppendRect(rect.left, rect.bottom, rect.right, rect.top);
  if (!SetClip_PathFill(path, nullptr,
                        CFX_FillRenderOptions::WindingOptions())) {
    return false;
  }

  UpdateClipBox();
  return true;
}

void CFX_RenderDevice::UpdateClipBox() {
  clip_box_ = device_driver_->GetClipBox();
}

bool CFX_RenderDevice::DrawPath(const CFX_Path& path,
                                const CFX_Matrix* pObject2Device,
                                const CFX_GraphStateData* pGraphState,
                                uint32_t fill_color,
                                uint32_t stroke_color,
                                const CFX_FillRenderOptions& fill_options) {
  const bool fill =
      fill_options.fill_type != CFX_FillRenderOptions::FillType::kNoFill;
  uint8_t fill_alpha = fill ? FXARGB_A(fill_color) : 0;
  uint8_t stroke_alpha = pGraphState ? FXARGB_A(stroke_color) : 0;
  pdfium::span<const CFX_Path::Point> points = path.GetPoints();
  if (stroke_alpha == 0 && points.size() == 2) {
    CFX_PointF pos1 = points[0].point_;
    CFX_PointF pos2 = points[1].point_;
    if (pObject2Device) {
      pos1 = pObject2Device->Transform(pos1);
      pos2 = pObject2Device->Transform(pos2);
    }
    DrawCosmeticLine(pos1, pos2, fill_color, fill_options);
    return true;
  }

  if (stroke_alpha == 0 && !fill_options.rect_aa) {
    std::optional<CFX_FloatRect> maybe_rect_f = path.GetRect(pObject2Device);
    if (maybe_rect_f.has_value()) {
      const CFX_FloatRect& rect_f = maybe_rect_f.value();
      FX_RECT rect_i = rect_f.GetOuterRect();

      // Depending on the top/bottom, left/right values of the rect it's
      // possible to overflow the Width() and Height() calculations. Check that
      // the rect will have valid dimension before continuing.
      if (!rect_i.Valid()) {
        return false;
      }

      int width = static_cast<int>(ceil(rect_f.right - rect_f.left));
      if (width < 1) {
        width = 1;
        if (rect_i.left == rect_i.right) {
          if (!pdfium::CheckAdd(rect_i.right, 1).AssignIfValid(&rect_i.right)) {
            return false;
          }
        }
      }
      int height = static_cast<int>(ceil(rect_f.top - rect_f.bottom));
      if (height < 1) {
        height = 1;
        if (rect_i.bottom == rect_i.top) {
          if (!pdfium::CheckAdd(rect_i.bottom, 1)
                   .AssignIfValid(&rect_i.bottom)) {
            return false;
          }
        }
      }
      if (rect_i.Width() >= width + 1) {
        if (rect_f.left - static_cast<float>(rect_i.left) >
            static_cast<float>(rect_i.right) - rect_f.right) {
          if (!pdfium::CheckAdd(rect_i.left, 1).AssignIfValid(&rect_i.left)) {
            return false;
          }
        } else {
          if (!pdfium::CheckSub(rect_i.right, 1).AssignIfValid(&rect_i.right)) {
            return false;
          }
        }
      }
      if (rect_i.Height() >= height + 1) {
        if (rect_f.top - static_cast<float>(rect_i.top) >
            static_cast<float>(rect_i.bottom) - rect_f.bottom) {
          if (!pdfium::CheckAdd(rect_i.top, 1).AssignIfValid(&rect_i.top)) {
            return false;
          }
        } else {
          if (!pdfium::CheckSub(rect_i.bottom, 1)
                   .AssignIfValid(&rect_i.bottom)) {
            return false;
          }
        }
      }
      if (FillRect(rect_i, fill_color)) {
        return true;
      }
    }
  }

  if (fill && stroke_alpha == 0 && !fill_options.stroke &&
      !fill_options.text_mode) {
    bool adjust = !!device_driver_->GetDriverType();
    std::vector<CFX_Path::Point> sub_path;
    for (size_t i = 0; i < points.size(); i++) {
      CFX_Path::Point::Type point_type = points[i].type_;
      if (point_type == CFX_Path::Point::Type::kMove) {
        // Process the existing sub path.
        DrawZeroAreaPath(sub_path, pObject2Device, adjust,
                         fill_options.aliased_path, fill_color, fill_alpha);
        sub_path.clear();

        // Start forming the next sub path.
        sub_path.push_back(points[i]);
        continue;
      }

      if (point_type == CFX_Path::Point::Type::kBezier) {
        sub_path.push_back(points[i]);
        sub_path.push_back(points[i + 1]);
        sub_path.push_back(points[i + 2]);
        i += 2;
        continue;
      }

      DCHECK_EQ(point_type, CFX_Path::Point::Type::kLine);
      sub_path.push_back(points[i]);
    }
    // Process the last sub paths.
    DrawZeroAreaPath(sub_path, pObject2Device, adjust,
                     fill_options.aliased_path, fill_color, fill_alpha);
  }

  if (fill && fill_alpha && stroke_alpha < 0xff && fill_options.stroke) {
#if defined(PDF_USE_SKIA)
    if (render_caps_ & FXRC_FILLSTROKE_PATH) {
      const bool using_skia = CFX_DefaultRenderDevice::UseSkiaRenderer();
      if (using_skia) {
        device_driver_->SetGroupKnockout(true);
      }
      bool draw_fillstroke_path_result =
          device_driver_->DrawPath(path, pObject2Device, pGraphState,
                                   fill_color, stroke_color, fill_options);

      if (using_skia) {
        // Restore the group knockout status for `device_driver_` after
        // finishing painting a fill-and-stroke path.
        device_driver_->SetGroupKnockout(false);
      }
      return draw_fillstroke_path_result;
    }
#endif  // defined(PDF_USE_SKIA)
    return DrawFillStrokePath(path, pObject2Device, pGraphState, fill_color,
                              stroke_color, fill_options);
  }
  return device_driver_->DrawPath(path, pObject2Device, pGraphState, fill_color,
                                  stroke_color, fill_options);
}

// This can be removed once PDFium entirely relies on Skia
bool CFX_RenderDevice::DrawFillStrokePath(
    const CFX_Path& path,
    const CFX_Matrix* pObject2Device,
    const CFX_GraphStateData* pGraphState,
    uint32_t fill_color,
    uint32_t stroke_color,
    const CFX_FillRenderOptions& fill_options) {
  if (!(render_caps_ & FXRC_GET_BITS)) {
    return false;
  }
  CFX_FloatRect bbox;
  if (pGraphState) {
    bbox = path.GetBoundingBoxForStrokePath(pGraphState->line_width(),
                                            pGraphState->miter_limit());
  } else {
    bbox = path.GetBoundingBox();
  }
  if (pObject2Device) {
    bbox = pObject2Device->TransformRect(bbox);
  }

  FX_RECT rect = bbox.GetOuterRect();
  if (!rect.Valid()) {
    return false;
  }

  auto bitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  auto backdrop = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!CreateCompatibleBitmap(bitmap, rect.Width(), rect.Height())) {
    return false;
  }

  if (bitmap->IsAlphaFormat()) {
    backdrop->Copy(bitmap);
  } else {
    if (!device_driver_->GetDIBits(bitmap, rect.left, rect.top)) {
      return false;
    }
    backdrop->Copy(bitmap);
  }
  CFX_DefaultRenderDevice bitmap_device;
  bitmap_device.AttachWithBackdropAndGroupKnockout(bitmap, std::move(backdrop),
                                                   /*bGroupKnockout=*/true);

  CFX_Matrix matrix;
  if (pObject2Device) {
    matrix = *pObject2Device;
  }
  matrix.Translate(-rect.left, -rect.top);
  if (!bitmap_device.GetDeviceDriver()->DrawPath(
          path, &matrix, pGraphState, fill_color, stroke_color, fill_options)) {
    return false;
  }
  FX_RECT src_rect(0, 0, rect.Width(), rect.Height());
  return device_driver_->SetDIBits(std::move(bitmap), /*color=*/0, src_rect,
                                   rect.left, rect.top, BlendMode::kNormal);
}

bool CFX_RenderDevice::FillRect(const FX_RECT& rect, uint32_t fill_color) {
  if (device_driver_->FillRect(rect, fill_color)) {
    return true;
  }

  if (!(render_caps_ & FXRC_GET_BITS)) {
    return false;
  }

  auto bitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!CreateCompatibleBitmap(bitmap, rect.Width(), rect.Height())) {
    return false;
  }

  if (!device_driver_->GetDIBits(bitmap, rect.left, rect.top)) {
    return false;
  }

  if (!bitmap->CompositeRect(0, 0, rect.Width(), rect.Height(), fill_color)) {
    return false;
  }

  FX_RECT src_rect(0, 0, rect.Width(), rect.Height());
  device_driver_->SetDIBits(std::move(bitmap), /*color=*/0, src_rect, rect.left,
                            rect.top, BlendMode::kNormal);
  return true;
}

bool CFX_RenderDevice::DrawCosmeticLine(
    const CFX_PointF& ptMoveTo,
    const CFX_PointF& ptLineTo,
    uint32_t color,
    const CFX_FillRenderOptions& fill_options) {
  if ((color >= 0xff000000) &&
      device_driver_->DrawCosmeticLine(ptMoveTo, ptLineTo, color)) {
    return true;
  }
  CFX_GraphStateData graph_state;
  CFX_Path path;
  path.AppendPoint(ptMoveTo, CFX_Path::Point::Type::kMove);
  path.AppendPoint(ptLineTo, CFX_Path::Point::Type::kLine);
  return device_driver_->DrawPath(path, nullptr, &graph_state, 0, color,
                                  fill_options);
}

void CFX_RenderDevice::DrawZeroAreaPath(
    const std::vector<CFX_Path::Point>& path,
    const CFX_Matrix* matrix,
    bool adjust,
    bool aliased_path,
    uint32_t fill_color,
    uint8_t fill_alpha) {
  if (path.empty()) {
    return;
  }

  CFX_Path new_path;
  bool thin = false;
  bool set_identity = false;

  if (!GetZeroAreaPath(path, matrix, adjust, &new_path, &thin, &set_identity)) {
    return;
  }

  CFX_GraphStateData graph_state;
  graph_state.set_line_width(0.0f);

  uint32_t stroke_color = fill_color;
  if (thin) {
    stroke_color = (((fill_alpha >> 2) << 24) | (stroke_color & 0x00ffffff));
  }

  const CFX_Matrix* new_matrix = nullptr;
  if (matrix && !matrix->IsIdentity() && !set_identity) {
    new_matrix = matrix;
  }

  CFX_FillRenderOptions path_options;
  path_options.zero_area = true;
  path_options.aliased_path = aliased_path;

  device_driver_->DrawPath(new_path, new_matrix, &graph_state, 0, stroke_color,
                           path_options);
}

bool CFX_RenderDevice::GetDIBits(RetainPtr<CFX_DIBitmap> bitmap,
                                 int left,
                                 int top) const {
  return (render_caps_ & FXRC_GET_BITS) &&
         device_driver_->GetDIBits(std::move(bitmap), left, top);
}

bool CFX_RenderDevice::SetDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                                 int left,
                                 int top) {
  return SetDIBitsWithBlend(std::move(bitmap), left, top, BlendMode::kNormal);
}

RetainPtr<const CFX_DIBitmap> CFX_RenderDevice::GetBackDrop() const {
  return device_driver_->GetBackDrop();
}

bool CFX_RenderDevice::SetDIBitsWithBlend(RetainPtr<const CFX_DIBBase> bitmap,
                                          int left,
                                          int top,
                                          BlendMode blend_mode) {
  DCHECK(!bitmap->IsMaskFormat());
  FX_RECT dest_rect(left, top, left + bitmap->GetWidth(),
                    top + bitmap->GetHeight());
  dest_rect.Intersect(clip_box_);
  if (dest_rect.IsEmpty()) {
    return true;
  }

  FX_RECT src_rect(dest_rect.left - left, dest_rect.top - top,
                   dest_rect.left - left + dest_rect.Width(),
                   dest_rect.top - top + dest_rect.Height());
  if ((blend_mode == BlendMode::kNormal || (render_caps_ & FXRC_BLEND_MODE)) &&
      (!bitmap->IsAlphaFormat() || (render_caps_ & FXRC_ALPHA_IMAGE))) {
    return device_driver_->SetDIBits(std::move(bitmap), /*color=*/0, src_rect,
                                     dest_rect.left, dest_rect.top, blend_mode);
  }
  if (!(render_caps_ & FXRC_GET_BITS)) {
    return false;
  }

  int bg_pixel_width = dest_rect.Width();
  int bg_pixel_height = dest_rect.Height();
  auto background = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!background->Create(bg_pixel_width, bg_pixel_height,
                          FXDIB_Format::kBgrx)) {
    return false;
  }
  if (!device_driver_->GetDIBits(background, dest_rect.left, dest_rect.top)) {
    return false;
  }

  if (!background->CompositeBitmap(0, 0, bg_pixel_width, bg_pixel_height,
                                   std::move(bitmap), src_rect.left,
                                   src_rect.top, blend_mode, nullptr, false)) {
    return false;
  }
  FX_RECT rect(0, 0, bg_pixel_width, bg_pixel_height);
  return device_driver_->SetDIBits(std::move(background), /*color=*/0, rect,
                                   dest_rect.left, dest_rect.top,
                                   BlendMode::kNormal);
}

bool CFX_RenderDevice::StretchDIBits(RetainPtr<const CFX_DIBBase> bitmap,
                                     int left,
                                     int top,
                                     int dest_width,
                                     int dest_height) {
  return StretchDIBitsWithFlagsAndBlend(
      std::move(bitmap), left, top, dest_width, dest_height,
      FXDIB_ResampleOptions(), BlendMode::kNormal);
}

bool CFX_RenderDevice::StretchDIBitsWithFlagsAndBlend(
    RetainPtr<const CFX_DIBBase> bitmap,
    int left,
    int top,
    int dest_width,
    int dest_height,
    const FXDIB_ResampleOptions& options,
    BlendMode blend_mode) {
  FX_RECT dest_rect(left, top, left + dest_width, top + dest_height);
  FX_RECT clip_box = clip_box_;
  clip_box.Intersect(dest_rect);
  return clip_box.IsEmpty() || device_driver_->StretchDIBits(
                                   std::move(bitmap), 0, left, top, dest_width,
                                   dest_height, &clip_box, options, blend_mode);
}

bool CFX_RenderDevice::SetBitMask(RetainPtr<const CFX_DIBBase> bitmap,
                                  int left,
                                  int top,
                                  uint32_t argb) {
  FX_RECT src_rect(0, 0, bitmap->GetWidth(), bitmap->GetHeight());
  return device_driver_->SetDIBits(std::move(bitmap), argb, src_rect, left, top,
                                   BlendMode::kNormal);
}

bool CFX_RenderDevice::StretchBitMask(RetainPtr<CFX_DIBBase> bitmap,
                                      int left,
                                      int top,
                                      int dest_width,
                                      int dest_height,
                                      uint32_t color) {
  return StretchBitMaskWithFlags(std::move(bitmap), left, top, dest_width,
                                 dest_height, color, FXDIB_ResampleOptions());
}

bool CFX_RenderDevice::StretchBitMaskWithFlags(
    RetainPtr<CFX_DIBBase> bitmap,
    int left,
    int top,
    int dest_width,
    int dest_height,
    uint32_t argb,
    const FXDIB_ResampleOptions& options) {
  FX_RECT dest_rect(left, top, left + dest_width, top + dest_height);
  FX_RECT clip_box = clip_box_;
  clip_box.Intersect(dest_rect);
  return device_driver_->StretchDIBits(std::move(bitmap), argb, left, top,
                                       dest_width, dest_height, &clip_box,
                                       options, BlendMode::kNormal);
}

RenderDeviceDriverIface::StartResult CFX_RenderDevice::StartDIBits(
    RetainPtr<const CFX_DIBBase> bitmap,
    float alpha,
    uint32_t argb,
    const CFX_Matrix& matrix,
    const FXDIB_ResampleOptions& options) {
  return StartDIBitsWithBlend(std::move(bitmap), alpha, argb, matrix, options,
                              BlendMode::kNormal);
}

RenderDeviceDriverIface::StartResult CFX_RenderDevice::StartDIBitsWithBlend(
    RetainPtr<const CFX_DIBBase> bitmap,
    float alpha,
    uint32_t argb,
    const CFX_Matrix& matrix,
    const FXDIB_ResampleOptions& options,
    BlendMode blend_mode) {
  return device_driver_->StartDIBits(std::move(bitmap), alpha, argb, matrix,
                                     options, blend_mode);
}

bool CFX_RenderDevice::ContinueDIBits(CFX_AggImageRenderer* handle,
                                      PauseIndicatorIface* pPause) {
  return device_driver_->ContinueDIBits(handle, pPause);
}

#if defined(PDF_USE_SKIA)
bool CFX_RenderDevice::DrawShading(const CPDF_ShadingPattern& pattern,
                                   const CFX_Matrix& matrix,
                                   const FX_RECT& clip_rect,
                                   int alpha) {
  return device_driver_->DrawShading(pattern, matrix, clip_rect, alpha);
}

bool CFX_RenderDevice::SetBitsWithMask(RetainPtr<const CFX_DIBBase> bitmap,
                                       RetainPtr<const CFX_DIBBase> mask,
                                       int left,
                                       int top,
                                       float alpha,
                                       BlendMode blend_type) {
  return device_driver_->SetBitsWithMask(std::move(bitmap), std::move(mask),
                                         left, top, alpha, blend_type);
}

void CFX_RenderDevice::SyncInternalBitmaps() {
  device_driver_->SyncInternalBitmaps();
}
#endif  // defined(PDF_USE_SKIA)

bool CFX_RenderDevice::DrawNormalText(pdfium::span<const TextCharPos> pCharPos,
                                      CFX_Font* font,
                                      float font_size,
                                      const CFX_Matrix& mtText2Device,
                                      uint32_t fill_color,
                                      const CFX_TextRenderOptions& options) {
  // `anti_alias` and `normalize` don't affect Skia rendering.
  int anti_alias = FT_RENDER_MODE_MONO;
  bool normalize = false;
  const bool is_text_smooth = options.IsSmooth();
  // |text_options| has the potential to affect all derived classes of
  // RenderDeviceDriverIface. But now it only affects Skia rendering.
  CFX_TextRenderOptions text_options(options);
  if (is_text_smooth) {
    if (GetDeviceType() == DeviceType::kDisplay && bpp_ > 1) {
      if (!CFX_GEModule::Get()->GetFontMgr()->FTLibrarySupportsHinting()) {
        // Some Freetype implementations (like the one packaged with Fedora) do
        // not support hinting due to patents 6219025, 6239783, 6307566,
        // 6225973, 6243070, 6393145, 6421054, 6282327, and 6624828; the latest
        // one expires 10/7/19.  This makes LCD anti-aliasing very ugly, so we
        // instead fall back on NORMAL anti-aliasing.
        anti_alias = FT_RENDER_MODE_NORMAL;
        if (CFX_DefaultRenderDevice::UseSkiaRenderer()) {
          // Since |anti_alias| doesn't affect Skia rendering, and Skia only
          // follows strictly to the options provided by |text_options|, we need
          // to update |text_options| so that Skia falls back on normal
          // anti-aliasing as well.
          text_options.aliasing_type = CFX_TextRenderOptions::kAntiAliasing;
        }
      } else if ((render_caps_ & FXRC_ALPHA_OUTPUT)) {
        // Whether Skia uses LCD optimization should strictly follow the
        // rendering options provided by |text_options|. No change needs to be
        // done for |text_options| here.
        anti_alias = FT_RENDER_MODE_LCD;
        normalize = true;
      } else if (bpp_ < 16) {
        // This case doesn't apply to Skia since Skia always have |bpp_| = 32.
        anti_alias = FT_RENDER_MODE_NORMAL;
      } else {
        // Whether Skia uses LCD optimization should strictly follow the
        // rendering options provided by |text_options|. No change needs to be
        // done for |text_options| here.
        anti_alias = FT_RENDER_MODE_LCD;
        normalize = !font->GetFaceRec() ||
                    options.aliasing_type != CFX_TextRenderOptions::kLcd;
      }
    }
  }

#if BUILDFLAG(IS_WIN)
  const bool is_printer = GetDeviceType() == DeviceType::kPrinter;
  bool try_native_text = true;
#else
  static constexpr bool is_printer = false;
  static constexpr bool try_native_text = true;
#endif

#if BUILDFLAG(IS_WIN)
  if (GetDeviceType() == DeviceType::kPrinter) {
    if (ShouldDrawDeviceText(font, options) &&
        device_driver_->DrawDeviceText(pCharPos, font, mtText2Device, font_size,
                                       fill_color, text_options)) {
      return true;
    }
    if (FXARGB_A(fill_color) < 255) {
      return false;
    }

    try_native_text = false;
  }
#endif

  if (try_native_text && options.native_text) {
    if (ShouldDrawDeviceText(font, options) &&
        device_driver_->DrawDeviceText(pCharPos, font, mtText2Device, font_size,
                                       fill_color, text_options)) {
      return true;
    }
  }

  CFX_Matrix char2device = mtText2Device;
  CFX_Matrix text2Device = mtText2Device;
  char2device.Scale(font_size, -font_size);
  if (fabs(char2device.a) + fabs(char2device.b) > 50 * 1.0f || is_printer) {
    if (font->GetFaceRec()) {
      CFX_FillRenderOptions path_options;
      path_options.aliased_path = !is_text_smooth;
      return DrawTextPath(pCharPos, font, font_size, mtText2Device, nullptr,
                          nullptr, fill_color, 0, nullptr, path_options);
    }
  }
  std::vector<TextGlyphPos> glyphs(pCharPos.size());
  for (auto [charpos, glyph] : fxcrt::Zip(pCharPos, pdfium::span(glyphs))) {
    glyph.device_origin_ = text2Device.Transform(charpos.origin_);
    glyph.origin_.x = anti_alias < FT_RENDER_MODE_LCD
                          ? FXSYS_roundf(glyph.device_origin_.x)
                          : static_cast<int>(floor(glyph.device_origin_.x));
    glyph.origin_.y = FXSYS_roundf(glyph.device_origin_.y);

    CFX_Matrix matrix = charpos.GetEffectiveMatrix(char2device);
    glyph.glyph_ = font->LoadGlyphBitmap(
        charpos.glyph_index_, charpos.font_style_, matrix,
        charpos.font_char_width_, anti_alias, &text_options);
  }
  if (anti_alias < FT_RENDER_MODE_LCD && glyphs.size() > 1) {
    AdjustGlyphSpace(&glyphs);
  }

  FX_RECT bmp_rect = GetGlyphsBBox(glyphs, anti_alias);
  bmp_rect.Intersect(clip_box_);
  if (bmp_rect.IsEmpty()) {
    return true;
  }

  int pixel_width = bmp_rect.Width();
  int pixel_height = bmp_rect.Height();
  int pixel_left = bmp_rect.left;
  int pixel_top = bmp_rect.top;
  if (anti_alias == FT_RENDER_MODE_MONO) {
    auto bitmap = pdfium::MakeRetain<CFX_DIBitmap>();
    if (!bitmap->Create(pixel_width, pixel_height, FXDIB_Format::k1bppMask)) {
      return false;
    }
    for (const TextGlyphPos& glyph : glyphs) {
      if (!glyph.glyph_) {
        continue;
      }

      std::optional<CFX_Point> point = glyph.GetOrigin({pixel_left, pixel_top});
      if (!point.has_value()) {
        continue;
      }

      const RetainPtr<CFX_DIBitmap>& pGlyph = glyph.glyph_->GetBitmap();
      bitmap->CompositeOneBPPMask(point.value().x, point.value().y,
                                  pGlyph->GetWidth(), pGlyph->GetHeight(),
                                  pGlyph, 0, 0);
    }
    return SetBitMask(std::move(bitmap), bmp_rect.left, bmp_rect.top,
                      fill_color);
  }
  auto bitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (bpp_ == 8) {
    if (!bitmap->Create(pixel_width, pixel_height, FXDIB_Format::k8bppMask)) {
      return false;
    }
  } else {
    // TODO(crbug.com/42271020): Switch to CreateCompatibleBitmap() once
    // DrawNormalTextHelper() supports `FXDIB_Format::kBgraPremul`.
    if (!bitmap->Create(pixel_width, pixel_height,
                        GetCreateCompatibleBitmapFormat(
                            render_caps_, /*use_argb_premul=*/false))) {
      return false;
    }
  }
  if (!bitmap->IsAlphaFormat() && !bitmap->IsMaskFormat()) {
    bitmap->Clear(0xFFFFFFFF);
    if (!GetDIBits(bitmap, bmp_rect.left, bmp_rect.top)) {
      return false;
    }
  }
  int dest_width = pixel_width;
  FX_BGRA_STRUCT<uint8_t> bgra;
  if (anti_alias == FT_RENDER_MODE_LCD) {
    bgra = ArgbToBGRAStruct(fill_color);
  }

  for (const TextGlyphPos& glyph : glyphs) {
    if (!glyph.glyph_) {
      continue;
    }

    std::optional<CFX_Point> point = glyph.GetOrigin({pixel_left, pixel_top});
    if (!point.has_value()) {
      continue;
    }

    const RetainPtr<CFX_DIBitmap>& pGlyph = glyph.glyph_->GetBitmap();
    int ncols = pGlyph->GetWidth();
    int nrows = pGlyph->GetHeight();
    if (anti_alias == FT_RENDER_MODE_NORMAL) {
      if (!bitmap->CompositeMask(point.value().x, point.value().y, ncols, nrows,
                                 pGlyph, fill_color, 0, 0, BlendMode::kNormal,
                                 nullptr, false)) {
        return false;
      }
      continue;
    }
    ncols /= 3;
    int x_subpixel = static_cast<int>(glyph.device_origin_.x * 3) % 3;
    int start_col = std::max(point->x, 0);
    FX_SAFE_INT32 end_col_safe = point->x;
    end_col_safe += ncols;
    if (!end_col_safe.IsValid()) {
      continue;
    }

    int end_col = std::min<int>(end_col_safe.ValueOrDie(), dest_width);
    if (start_col >= end_col) {
      continue;
    }

    DrawNormalTextHelper(bitmap, pGlyph, nrows, point->x, point->y, start_col,
                         end_col, normalize, x_subpixel, bgra);
  }

  if (bitmap->IsMaskFormat()) {
    SetBitMask(std::move(bitmap), bmp_rect.left, bmp_rect.top, fill_color);
  } else {
    SetDIBits(std::move(bitmap), bmp_rect.left, bmp_rect.top);
  }
  return true;
}

bool CFX_RenderDevice::DrawTextPath(pdfium::span<const TextCharPos> pCharPos,
                                    CFX_Font* font,
                                    float font_size,
                                    const CFX_Matrix& mtText2User,
                                    const CFX_Matrix* pUser2Device,
                                    const CFX_GraphStateData* pGraphState,
                                    uint32_t fill_color,
                                    FX_ARGB stroke_color,
                                    CFX_Path* pClippingPath,
                                    const CFX_FillRenderOptions& fill_options) {
  for (const auto& charpos : pCharPos) {
    const CFX_Path* pPath =
        font->LoadGlyphPath(charpos.glyph_index_, charpos.font_char_width_);
    if (!pPath) {
      continue;
    }

    CFX_Matrix matrix(font_size, 0, 0, font_size, charpos.origin_.x,
                      charpos.origin_.y);
    matrix = charpos.GetEffectiveMatrix(matrix);
    matrix.Concat(mtText2User);

    CFX_Path transformed_path(*pPath);
    transformed_path.Transform(matrix);
    if (fill_color || stroke_color) {
      CFX_FillRenderOptions options(fill_options);
      if (fill_color) {
        options.fill_type = CFX_FillRenderOptions::FillType::kWinding;
      }
      options.text_mode = true;
      if (!DrawPath(transformed_path, pUser2Device, pGraphState, fill_color,
                    stroke_color, options)) {
        return false;
      }
    }
    if (pClippingPath) {
      pClippingPath->Append(transformed_path, pUser2Device);
    }
  }
  return true;
}

void CFX_RenderDevice::DrawFillRect(const CFX_Matrix* pUser2Device,
                                    const CFX_FloatRect& rect,
                                    const FX_COLORREF& color) {
  CFX_Path path;
  path.AppendFloatRect(rect);
  DrawPath(path, pUser2Device, nullptr, color, 0,
           CFX_FillRenderOptions::WindingOptions());
}

void CFX_RenderDevice::DrawFillArea(const CFX_Matrix& mtUser2Device,
                                    const std::vector<CFX_PointF>& points,
                                    const FX_COLORREF& color) {
  DCHECK(!points.empty());
  CFX_Path path;
  path.AppendPoint(points[0], CFX_Path::Point::Type::kMove);
  for (size_t i = 1; i < points.size(); ++i) {
    path.AppendPoint(points[i], CFX_Path::Point::Type::kLine);
  }

  DrawPath(path, &mtUser2Device, nullptr, color, 0,
           CFX_FillRenderOptions::EvenOddOptions());
}

void CFX_RenderDevice::DrawStrokeRect(const CFX_Matrix& mtUser2Device,
                                      const CFX_FloatRect& rect,
                                      const FX_COLORREF& color,
                                      float fWidth) {
  CFX_GraphStateData gsd;
  gsd.set_line_width(fWidth);

  CFX_Path path;
  path.AppendFloatRect(rect);
  DrawPath(path, &mtUser2Device, &gsd, 0, color,
           CFX_FillRenderOptions::EvenOddOptions());
}

void CFX_RenderDevice::DrawStrokeLine(const CFX_Matrix* pUser2Device,
                                      const CFX_PointF& ptMoveTo,
                                      const CFX_PointF& ptLineTo,
                                      const FX_COLORREF& color,
                                      float fWidth) {
  CFX_Path path;
  path.AppendPoint(ptMoveTo, CFX_Path::Point::Type::kMove);
  path.AppendPoint(ptLineTo, CFX_Path::Point::Type::kLine);

  CFX_GraphStateData gsd;
  gsd.set_line_width(fWidth);

  DrawPath(path, pUser2Device, &gsd, 0, color,
           CFX_FillRenderOptions::EvenOddOptions());
}

void CFX_RenderDevice::DrawFillRect(const CFX_Matrix* pUser2Device,
                                    const CFX_FloatRect& rect,
                                    const CFX_Color& color,
                                    int32_t nTransparency) {
  DrawFillRect(pUser2Device, rect, color.ToFXColor(nTransparency));
}

void CFX_RenderDevice::DrawShadow(const CFX_Matrix& mtUser2Device,
                                  const CFX_FloatRect& rect,
                                  int32_t nTransparency,
                                  int32_t nStartGray,
                                  int32_t nEndGray) {
  static constexpr float kBorder = 0.5f;
  static constexpr float kSegmentWidth = 1.0f;
  static constexpr float kLineWidth = 1.5f;

  float fStepGray = (nEndGray - nStartGray) / rect.Height();
  CFX_PointF start(rect.left, 0);
  CFX_PointF end(rect.right, 0);

  for (float fy = rect.bottom + kBorder; fy <= rect.top - kBorder;
       fy += kSegmentWidth) {
    start.y = fy;
    end.y = fy;
    int nGray = nStartGray + static_cast<int>(fStepGray * (fy - rect.bottom));
    FX_ARGB color = ArgbEncode(nTransparency, nGray, nGray, nGray);
    DrawStrokeLine(&mtUser2Device, start, end, color, kLineWidth);
  }
}

void CFX_RenderDevice::DrawBorder(const CFX_Matrix* pUser2Device,
                                  const CFX_FloatRect& rect,
                                  float fWidth,
                                  const CFX_Color& color,
                                  const CFX_Color& crLeftTop,
                                  const CFX_Color& crRightBottom,
                                  BorderStyle nStyle,
                                  int32_t nTransparency) {
  if (fWidth <= 0.0f) {
    return;
  }

  const float fLeft = rect.left;
  const float fRight = rect.right;
  const float fTop = rect.top;
  const float fBottom = rect.bottom;
  const float fHalfWidth = fWidth / 2.0f;

  switch (nStyle) {
    case BorderStyle::kSolid: {
      CFX_Path path;
      path.AppendRect(fLeft, fBottom, fRight, fTop);
      path.AppendRect(fLeft + fWidth, fBottom + fWidth, fRight - fWidth,
                      fTop - fWidth);
      DrawPath(path, pUser2Device, nullptr, color.ToFXColor(nTransparency), 0,
               CFX_FillRenderOptions::EvenOddOptions());
      break;
    }
    case BorderStyle::kDash: {
      CFX_GraphStateData gsd;
      gsd.set_dash_array({3.0f, 3.0f});
      gsd.set_line_width(fWidth);

      CFX_Path path;
      path.AppendPoint(CFX_PointF(fLeft + fHalfWidth, fBottom + fHalfWidth),
                       CFX_Path::Point::Type::kMove);
      path.AppendPoint(CFX_PointF(fLeft + fHalfWidth, fTop - fHalfWidth),
                       CFX_Path::Point::Type::kLine);
      path.AppendPoint(CFX_PointF(fRight - fHalfWidth, fTop - fHalfWidth),
                       CFX_Path::Point::Type::kLine);
      path.AppendPoint(CFX_PointF(fRight - fHalfWidth, fBottom + fHalfWidth),
                       CFX_Path::Point::Type::kLine);
      path.AppendPoint(CFX_PointF(fLeft + fHalfWidth, fBottom + fHalfWidth),
                       CFX_Path::Point::Type::kLine);
      DrawPath(path, pUser2Device, &gsd, 0, color.ToFXColor(nTransparency),
               CFX_FillRenderOptions::WindingOptions());
      break;
    }
    case BorderStyle::kBeveled:
    case BorderStyle::kInset: {
      CFX_GraphStateData gsd;
      gsd.set_line_width(fHalfWidth);

      CFX_Path path_left_top;
      path_left_top.AppendPoint(
          CFX_PointF(fLeft + fHalfWidth, fBottom + fHalfWidth),
          CFX_Path::Point::Type::kMove);
      path_left_top.AppendPoint(
          CFX_PointF(fLeft + fHalfWidth, fTop - fHalfWidth),
          CFX_Path::Point::Type::kLine);
      path_left_top.AppendPoint(
          CFX_PointF(fRight - fHalfWidth, fTop - fHalfWidth),
          CFX_Path::Point::Type::kLine);
      path_left_top.AppendPoint(CFX_PointF(fRight - fWidth, fTop - fWidth),
                                CFX_Path::Point::Type::kLine);
      path_left_top.AppendPoint(CFX_PointF(fLeft + fWidth, fTop - fWidth),
                                CFX_Path::Point::Type::kLine);
      path_left_top.AppendPoint(CFX_PointF(fLeft + fWidth, fBottom + fWidth),
                                CFX_Path::Point::Type::kLine);
      path_left_top.AppendPoint(
          CFX_PointF(fLeft + fHalfWidth, fBottom + fHalfWidth),
          CFX_Path::Point::Type::kLine);
      DrawPath(path_left_top, pUser2Device, &gsd,
               crLeftTop.ToFXColor(nTransparency), 0,
               CFX_FillRenderOptions::EvenOddOptions());

      CFX_Path path_right_bottom;
      path_right_bottom.AppendPoint(
          CFX_PointF(fRight - fHalfWidth, fTop - fHalfWidth),
          CFX_Path::Point::Type::kMove);
      path_right_bottom.AppendPoint(
          CFX_PointF(fRight - fHalfWidth, fBottom + fHalfWidth),
          CFX_Path::Point::Type::kLine);
      path_right_bottom.AppendPoint(
          CFX_PointF(fLeft + fHalfWidth, fBottom + fHalfWidth),
          CFX_Path::Point::Type::kLine);
      path_right_bottom.AppendPoint(
          CFX_PointF(fLeft + fWidth, fBottom + fWidth),
          CFX_Path::Point::Type::kLine);
      path_right_bottom.AppendPoint(
          CFX_PointF(fRight - fWidth, fBottom + fWidth),
          CFX_Path::Point::Type::kLine);
      path_right_bottom.AppendPoint(CFX_PointF(fRight - fWidth, fTop - fWidth),
                                    CFX_Path::Point::Type::kLine);
      path_right_bottom.AppendPoint(
          CFX_PointF(fRight - fHalfWidth, fTop - fHalfWidth),
          CFX_Path::Point::Type::kLine);
      DrawPath(path_right_bottom, pUser2Device, &gsd,
               crRightBottom.ToFXColor(nTransparency), 0,
               CFX_FillRenderOptions::EvenOddOptions());

      CFX_Path path;
      path.AppendRect(fLeft, fBottom, fRight, fTop);
      path.AppendRect(fLeft + fHalfWidth, fBottom + fHalfWidth,
                      fRight - fHalfWidth, fTop - fHalfWidth);
      DrawPath(path, pUser2Device, &gsd, color.ToFXColor(nTransparency), 0,
               CFX_FillRenderOptions::EvenOddOptions());
      break;
    }
    case BorderStyle::kUnderline: {
      CFX_GraphStateData gsd;
      gsd.set_line_width(fWidth);

      CFX_Path path;
      path.AppendPoint(CFX_PointF(fLeft, fBottom + fHalfWidth),
                       CFX_Path::Point::Type::kMove);
      path.AppendPoint(CFX_PointF(fRight, fBottom + fHalfWidth),
                       CFX_Path::Point::Type::kLine);
      DrawPath(path, pUser2Device, &gsd, 0, color.ToFXColor(nTransparency),
               CFX_FillRenderOptions::EvenOddOptions());
      break;
    }
  }
}

bool CFX_RenderDevice::MultiplyAlpha(float alpha) {
  return device_driver_->MultiplyAlpha(alpha);
}

bool CFX_RenderDevice::MultiplyAlphaMask(RetainPtr<const CFX_DIBitmap> mask) {
  return device_driver_->MultiplyAlphaMask(std::move(mask));
}

CFX_RenderDevice::StateRestorer::StateRestorer(CFX_RenderDevice* pDevice)
    : device_(pDevice) {
  device_->SaveState();
}

CFX_RenderDevice::StateRestorer::~StateRestorer() {
  device_->RestoreState(false);
}
