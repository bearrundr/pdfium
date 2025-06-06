// Copyright 2017 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "xfa/fgas/layout/cfgas_break.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/stl_util.h"
#include "xfa/fgas/font/cfgas_gefont.h"

const float CFGAS_Break::kConversionFactor = 20000.0f;
const int CFGAS_Break::kMinimumTabWidth = 160000;

CFGAS_Break::CFGAS_Break(Mask<LayoutStyle> dwLayoutStyles)
    : layout_styles_(dwLayoutStyles), cur_line_(&lines_[0]) {}

CFGAS_Break::~CFGAS_Break() = default;

void CFGAS_Break::Reset() {
  char_type_ = FX_CHARTYPE::kUnknown;
  for (CFGAS_BreakLine& line : lines_) {
    line.Clear();
  }
}

void CFGAS_Break::SetLayoutStyles(Mask<LayoutStyle> dwLayoutStyles) {
  layout_styles_ = dwLayoutStyles;
  single_line_ = !!(layout_styles_ & LayoutStyle::kSingleLine);
  comb_text_ = !!(layout_styles_ & LayoutStyle::kCombText);
}

void CFGAS_Break::SetHorizontalScale(int32_t iScale) {
  iScale = std::max(iScale, 0);
  if (horizontal_scale_ == iScale) {
    return;
  }

  SetBreakStatus();
  horizontal_scale_ = iScale;
}

void CFGAS_Break::SetVerticalScale(int32_t iScale) {
  if (iScale < 0) {
    iScale = 0;
  }
  if (vertical_scale_ == iScale) {
    return;
  }

  SetBreakStatus();
  vertical_scale_ = iScale;
}

void CFGAS_Break::SetFont(RetainPtr<CFGAS_GEFont> font) {
  if (!font || font == font_) {
    return;
  }

  SetBreakStatus();
  font_ = std::move(font);
}

void CFGAS_Break::SetFontSize(float fFontSize) {
  int32_t iFontSize = FXSYS_roundf(fFontSize * 20.0f);
  if (font_size_ == iFontSize) {
    return;
  }

  SetBreakStatus();
  font_size_ = iFontSize;
}

void CFGAS_Break::SetBreakStatus() {
  ++identity_;

  CFGAS_Char* tc = cur_line_->LastChar();
  if (tc && tc->status_ == CFGAS_Char::BreakType::kNone) {
    tc->status_ = CFGAS_Char::BreakType::kPiece;
  }
}

bool CFGAS_Break::IsGreaterThanLineWidth(int32_t width) const {
  FX_SAFE_INT32 line_width = line_width_;
  line_width += tolerance_;
  return line_width.IsValid() && width > line_width.ValueOrDie();
}

FX_CHARTYPE CFGAS_Break::GetUnifiedCharType(FX_CHARTYPE chartype) const {
  return chartype >= FX_CHARTYPE::kArabicAlef ? FX_CHARTYPE::kArabic : chartype;
}

void CFGAS_Break::SetTabWidth(float fTabWidth) {
  // Note, the use of max here was only done in the TxtBreak code. Leaving this
  // in for the RTFBreak code for consistency. If we see issues with tab widths
  // we may need to fix this.
  tab_width_ =
      std::max(FXSYS_roundf(fTabWidth * kConversionFactor), kMinimumTabWidth);
}

void CFGAS_Break::SetParagraphBreakChar(wchar_t wch) {
  if (wch != L'\r' && wch != L'\n') {
    return;
  }
  w_paragraph_break_char_ = wch;
}

void CFGAS_Break::SetLineBreakTolerance(float fTolerance) {
  tolerance_ = FXSYS_roundf(fTolerance * kConversionFactor);
}

void CFGAS_Break::SetCharSpace(float fCharSpace) {
  char_space_ = FXSYS_roundf(fCharSpace * kConversionFactor);
}

void CFGAS_Break::SetLineBoundary(float fLineStart, float fLineEnd) {
  if (fLineStart > fLineEnd) {
    return;
  }

  line_start_ = FXSYS_roundf(fLineStart * kConversionFactor);
  line_width_ = FXSYS_roundf(fLineEnd * kConversionFactor);
  cur_line_->start_ = std::min(cur_line_->start_, line_width_);
  cur_line_->start_ = std::max(cur_line_->start_, line_start_);
}

CFGAS_Char* CFGAS_Break::GetLastChar(int32_t index,
                                     bool bOmitChar,
                                     bool bRichText) const {
  std::vector<CFGAS_Char>& tca = cur_line_->line_chars_;
  if (!fxcrt::IndexInBounds(tca, index)) {
    return nullptr;
  }

  int32_t iStart = fxcrt::CollectionSize<int32_t>(tca) - 1;
  while (iStart > -1) {
    CFGAS_Char* pTC = &tca[iStart--];
    if (((bRichText && pTC->char_width_ < 0) || bOmitChar) &&
        pTC->GetCharType() == FX_CHARTYPE::kCombination) {
      continue;
    }
    if (--index < 0) {
      return pTC;
    }
  }
  return nullptr;
}

int32_t CFGAS_Break::CountBreakPieces() const {
  return HasLine() ? fxcrt::CollectionSize<int32_t>(
                         lines_[ready_line_index_].line_pieces_)
                   : 0;
}

const CFGAS_BreakPiece* CFGAS_Break::GetBreakPieceUnstable(
    int32_t index) const {
  if (!HasLine()) {
    return nullptr;
  }
  if (!fxcrt::IndexInBounds(lines_[ready_line_index_].line_pieces_, index)) {
    return nullptr;
  }
  return &lines_[ready_line_index_].line_pieces_[index];
}

void CFGAS_Break::ClearBreakPieces() {
  if (HasLine()) {
    lines_[ready_line_index_].Clear();
  }
  ready_line_index_ = -1;
}
