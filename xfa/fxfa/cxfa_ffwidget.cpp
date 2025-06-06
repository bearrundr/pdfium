// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "xfa/fxfa/cxfa_ffwidget.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "core/fxcodec/fx_codec.h"
#include "core/fxcodec/progressive_decoder.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/maybe_owned.h"
#include "core/fxge/cfx_fillrenderoptions.h"
#include "core/fxge/cfx_path.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "xfa/fgas/graphics/cfgas_gegraphics.h"
#include "xfa/fwl/fwl_widgethit.h"
#include "xfa/fxfa/cxfa_eventparam.h"
#include "xfa/fxfa/cxfa_ffapp.h"
#include "xfa/fxfa/cxfa_ffdoc.h"
#include "xfa/fxfa/cxfa_ffdocview.h"
#include "xfa/fxfa/cxfa_ffpageview.h"
#include "xfa/fxfa/cxfa_ffwidgethandler.h"
#include "xfa/fxfa/cxfa_imagerenderer.h"
#include "xfa/fxfa/layout/cxfa_layoutprocessor.h"
#include "xfa/fxfa/parser/cxfa_border.h"
#include "xfa/fxfa/parser/cxfa_box.h"
#include "xfa/fxfa/parser/cxfa_edge.h"
#include "xfa/fxfa/parser/cxfa_image.h"
#include "xfa/fxfa/parser/cxfa_margin.h"
#include "xfa/fxfa/parser/cxfa_node.h"

void XFA_DrawImage(CFGAS_GEGraphics* pGS,
                   const CFX_RectF& rtImage,
                   const CFX_Matrix& matrix,
                   RetainPtr<CFX_DIBitmap> bitmap,
                   XFA_AttributeValue iAspect,
                   const CFX_Size& dpi,
                   XFA_AttributeValue iHorzAlign,
                   XFA_AttributeValue iVertAlign) {
  if (rtImage.IsEmpty()) {
    return;
  }

  CHECK(bitmap);
  if (bitmap->GetBuffer().empty()) {
    return;
  }

  CFX_RectF rtFit(rtImage.TopLeft(),
                  XFA_UnitPx2Pt(bitmap->GetWidth(), dpi.width),
                  XFA_UnitPx2Pt(bitmap->GetHeight(), dpi.height));
  switch (iAspect) {
    case XFA_AttributeValue::Fit: {
      float f1 = rtImage.height / rtFit.height;
      float f2 = rtImage.width / rtFit.width;
      f1 = std::min(f1, f2);
      rtFit.height = rtFit.height * f1;
      rtFit.width = rtFit.width * f1;
      break;
    }
    case XFA_AttributeValue::Height: {
      float f1 = rtImage.height / rtFit.height;
      rtFit.height = rtImage.height;
      rtFit.width = f1 * rtFit.width;
      break;
    }
    case XFA_AttributeValue::None:
      rtFit.height = rtImage.height;
      rtFit.width = rtImage.width;
      break;
    case XFA_AttributeValue::Width: {
      float f1 = rtImage.width / rtFit.width;
      rtFit.width = rtImage.width;
      rtFit.height = rtFit.height * f1;
      break;
    }
    case XFA_AttributeValue::Actual:
    default:
      break;
  }

  if (iHorzAlign == XFA_AttributeValue::Center) {
    rtFit.left += (rtImage.width - rtFit.width) / 2;
  } else if (iHorzAlign == XFA_AttributeValue::Right) {
    rtFit.left = rtImage.right() - rtFit.width;
  }

  if (iVertAlign == XFA_AttributeValue::Middle) {
    rtFit.top += (rtImage.height - rtFit.height) / 2;
  } else if (iVertAlign == XFA_AttributeValue::Bottom) {
    rtFit.top = rtImage.bottom() - rtImage.height;
  }

  CFX_RenderDevice* device = pGS->GetRenderDevice();
  CFX_RenderDevice::StateRestorer restorer(device);
  CFX_Path path;
  path.AppendRect(rtImage.left, rtImage.bottom(), rtImage.right(), rtImage.top);
  device->SetClip_PathFill(path, &matrix,
                           CFX_FillRenderOptions::WindingOptions());

  CFX_Matrix image_to_device(1, 0, 0, -1, 0, 1);
  image_to_device.Concat(
      CFX_Matrix(rtFit.width, 0, 0, rtFit.height, rtFit.left, rtFit.top));
  image_to_device.Concat(matrix);

  CXFA_ImageRenderer image_renderer(device, std::move(bitmap), image_to_device);
  if (!image_renderer.Start()) {
    return;
  }

  while (image_renderer.Continue()) {
    continue;
  }
}

RetainPtr<CFX_DIBitmap> XFA_LoadImageFromBuffer(
    RetainPtr<IFX_SeekableReadStream> pImageFileRead,
    FXCODEC_IMAGE_TYPE type,
    int32_t& iImageXDpi,
    int32_t& iImageYDpi) {
  auto pProgressiveDecoder = std::make_unique<ProgressiveDecoder>();

  CFX_DIBAttribute dibAttr;
  pProgressiveDecoder->LoadImageInfo(std::move(pImageFileRead), type, &dibAttr,
                                     false);
  switch (dibAttr.dpi_unit_) {
    case CFX_DIBAttribute::kResUnitCentimeter:
      dibAttr.x_dpi_ = static_cast<int32_t>(dibAttr.x_dpi_ * 2.54f);
      dibAttr.y_dpi_ = static_cast<int32_t>(dibAttr.y_dpi_ * 2.54f);
      break;
    case CFX_DIBAttribute::kResUnitMeter:
      dibAttr.x_dpi_ =
          static_cast<int32_t>(dibAttr.x_dpi_ / (float)100 * 2.54f);
      dibAttr.y_dpi_ =
          static_cast<int32_t>(dibAttr.y_dpi_ / (float)100 * 2.54f);
      break;
    default:
      break;
  }
  iImageXDpi = dibAttr.x_dpi_ > 1 ? dibAttr.x_dpi_ : (96);
  iImageYDpi = dibAttr.y_dpi_ > 1 ? dibAttr.y_dpi_ : (96);
  if (pProgressiveDecoder->GetWidth() <= 0 ||
      pProgressiveDecoder->GetHeight() <= 0) {
    return nullptr;
  }

  RetainPtr<CFX_DIBitmap> pBitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!pBitmap->Create(pProgressiveDecoder->GetWidth(),
                       pProgressiveDecoder->GetHeight(),
                       pProgressiveDecoder->GetBitmapFormat())) {
    return nullptr;
  }

  pBitmap->Clear(0xffffffff);

  auto [status, nFrames] = pProgressiveDecoder->GetFrames();
  if (status != FXCODEC_STATUS::kDecodeReady || nFrames == 0) {
    return nullptr;
  }

  status = pProgressiveDecoder->StartDecode(pBitmap);
  if (status == FXCODEC_STATUS::kError) {
    return nullptr;
  }

  while (status == FXCODEC_STATUS::kDecodeToBeContinued) {
    status = pProgressiveDecoder->ContinueDecode();
    if (status == FXCODEC_STATUS::kError) {
      return nullptr;
    }
  }

  return pBitmap;
}

void XFA_RectWithoutMargin(CFX_RectF* rt, const CXFA_Margin* margin) {
  if (!margin) {
    return;
  }

  rt->Deflate(margin->GetLeftInset(), margin->GetTopInset(),
              margin->GetRightInset(), margin->GetBottomInset());
}

// static
CXFA_FFWidget* CXFA_FFWidget::FromLayoutItem(CXFA_LayoutItem* pLayoutItem) {
  if (!pLayoutItem->GetFormNode()->HasCreatedUIWidget()) {
    return nullptr;
  }

  return GetFFWidget(ToContentLayoutItem(pLayoutItem));
}

CXFA_FFWidget::CXFA_FFWidget(CXFA_Node* node) : node_(node) {}

CXFA_FFWidget::~CXFA_FFWidget() = default;

void CXFA_FFWidget::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(layout_item_);
  visitor->Trace(doc_view_);
  visitor->Trace(page_view_);
  visitor->Trace(node_);
}

CFWL_App* CXFA_FFWidget::GetFWLApp() const {
  return GetPageView()->GetDocView()->GetDoc()->GetApp()->GetFWLApp();
}

CXFA_FFWidget* CXFA_FFWidget::GetNextFFWidget() const {
  return GetFFWidget(GetLayoutItem()->GetNext());
}

const CFX_RectF& CXFA_FFWidget::GetWidgetRect() const {
  if (!GetLayoutItem()->TestStatusBits(XFA_WidgetStatus::kRectCached)) {
    RecacheWidgetRect();
  }
  return widget_rect_;
}

const CFX_RectF& CXFA_FFWidget::RecacheWidgetRect() const {
  GetLayoutItem()->SetStatusBits(XFA_WidgetStatus::kRectCached);
  widget_rect_ = GetLayoutItem()->GetAbsoluteRect();
  return widget_rect_;
}

CFX_RectF CXFA_FFWidget::GetRectWithoutRotate() {
  CFX_RectF rtWidget = GetWidgetRect();
  float fValue = 0;
  switch (node_->GetRotate()) {
    case 90:
      rtWidget.top = rtWidget.bottom();
      fValue = rtWidget.width;
      rtWidget.width = rtWidget.height;
      rtWidget.height = fValue;
      break;
    case 180:
      rtWidget.left = rtWidget.right();
      rtWidget.top = rtWidget.bottom();
      break;
    case 270:
      rtWidget.left = rtWidget.right();
      fValue = rtWidget.width;
      rtWidget.width = rtWidget.height;
      rtWidget.height = fValue;
      break;
  }
  return rtWidget;
}

void CXFA_FFWidget::ModifyStatus(Mask<XFA_WidgetStatus> dwAdded,
                                 Mask<XFA_WidgetStatus> dwRemoved) {
  GetLayoutItem()->ClearStatusBits(dwRemoved);
  GetLayoutItem()->SetStatusBits(dwAdded);
}

CXFA_FFField* CXFA_FFWidget::AsField() {
  return nullptr;
}

CFX_RectF CXFA_FFWidget::GetBBox(FocusOption focus) {
  if (focus == kDrawFocus || !page_view_) {
    return CFX_RectF();
  }
  return page_view_->GetPageViewRect();
}

void CXFA_FFWidget::RenderWidget(CFGAS_GEGraphics* pGS,
                                 const CFX_Matrix& matrix,
                                 HighlightOption highlight) {
  if (!HasVisibleStatus()) {
    return;
  }

  CXFA_Border* border = node_->GetBorderIfExists();
  if (!border) {
    return;
  }

  CFX_RectF rtBorder = GetRectWithoutRotate();
  CXFA_Margin* margin = border->GetMarginIfExists();
  XFA_RectWithoutMargin(&rtBorder, margin);
  rtBorder.Normalize();
  DrawBorder(pGS, border, rtBorder, matrix);
}

bool CXFA_FFWidget::IsLoaded() {
  return !!page_view_;
}

bool CXFA_FFWidget::LoadWidget() {
  PerformLayout();
  return true;
}

void CXFA_FFWidget::PerformLayout() {
  RecacheWidgetRect();
}

void CXFA_FFWidget::UpdateFWLData() {}

void CXFA_FFWidget::UpdateWidgetProperty() {}

bool CXFA_FFWidget::HasEventUnderHandler(XFA_EVENTTYPE eEventType,
                                         CXFA_FFWidgetHandler* pHandler) {
  CXFA_Node* pNode = GetNode();
  return pNode->IsWidgetReady() && pHandler->HasEvent(pNode, eEventType);
}

bool CXFA_FFWidget::ProcessEventUnderHandler(CXFA_EventParam* params,
                                             CXFA_FFWidgetHandler* pHandler) {
  CXFA_Node* pNode = GetNode();
  if (!pNode->IsWidgetReady()) {
    return false;
  }

  return pHandler->ProcessEvent(pNode, params) == XFA_EventError::kSuccess;
}

void CXFA_FFWidget::DrawBorder(CFGAS_GEGraphics* pGS,
                               CXFA_Box* box,
                               const CFX_RectF& rtBorder,
                               const CFX_Matrix& matrix) {
  if (box) {
    box->Draw(pGS, rtBorder, matrix, false);
  }
}

void CXFA_FFWidget::DrawBorderWithFlag(CFGAS_GEGraphics* pGS,
                                       CXFA_Box* box,
                                       const CFX_RectF& rtBorder,
                                       const CFX_Matrix& matrix,
                                       bool forceRound) {
  if (box) {
    box->Draw(pGS, rtBorder, matrix, forceRound);
  }
}

void CXFA_FFWidget::InvalidateRect() {
  CFX_RectF rtWidget = GetBBox(kDoNotDrawFocus);
  rtWidget.Inflate(2, 2);
  doc_view_->InvalidateRect(page_view_.Get(), rtWidget);
}

bool CXFA_FFWidget::OnMouseEnter() {
  return false;
}

bool CXFA_FFWidget::OnMouseExit() {
  return false;
}

bool CXFA_FFWidget::AcceptsFocusOnButtonDown(
    Mask<XFA_FWL_KeyFlag> dwFlags,
    const CFX_PointF& point,
    CFWL_MessageMouse::MouseCommand command) {
  return false;
}

bool CXFA_FFWidget::OnLButtonDown(Mask<XFA_FWL_KeyFlag> dwFlags,
                                  const CFX_PointF& point) {
  return false;
}

bool CXFA_FFWidget::OnLButtonUp(Mask<XFA_FWL_KeyFlag> dwFlags,
                                const CFX_PointF& point) {
  return false;
}

bool CXFA_FFWidget::OnLButtonDblClk(Mask<XFA_FWL_KeyFlag> dwFlags,
                                    const CFX_PointF& point) {
  return false;
}

bool CXFA_FFWidget::OnMouseMove(Mask<XFA_FWL_KeyFlag> dwFlags,
                                const CFX_PointF& point) {
  return false;
}

bool CXFA_FFWidget::OnMouseWheel(Mask<XFA_FWL_KeyFlag> dwFlags,
                                 const CFX_PointF& point,
                                 const CFX_Vector& delta) {
  return false;
}

bool CXFA_FFWidget::OnRButtonDown(Mask<XFA_FWL_KeyFlag> dwFlags,
                                  const CFX_PointF& point) {
  return false;
}

bool CXFA_FFWidget::OnRButtonUp(Mask<XFA_FWL_KeyFlag> dwFlags,
                                const CFX_PointF& point) {
  return false;
}

bool CXFA_FFWidget::OnRButtonDblClk(Mask<XFA_FWL_KeyFlag> dwFlags,
                                    const CFX_PointF& point) {
  return false;
}

bool CXFA_FFWidget::OnSetFocus(CXFA_FFWidget* pOldWidget) {
  CXFA_FFWidget* pParent = GetFFWidget(ToContentLayoutItem(GetParent()));
  if (pParent && !pParent->IsAncestorOf(pOldWidget)) {
    if (!pParent->OnSetFocus(pOldWidget)) {
      return false;
    }
  }
  GetLayoutItem()->SetStatusBits(XFA_WidgetStatus::kFocused);

  CXFA_EventParam eParam(XFA_EVENT_Enter);
  node_->ProcessEvent(GetDocView(), XFA_AttributeValue::Enter, &eParam);
  return true;
}

bool CXFA_FFWidget::OnKillFocus(CXFA_FFWidget* pNewWidget) {
  GetLayoutItem()->ClearStatusBits(XFA_WidgetStatus::kFocused);
  EventKillFocus();
  if (!pNewWidget) {
    return true;
  }

  CXFA_FFWidget* pParent = GetFFWidget(ToContentLayoutItem(GetParent()));
  if (pParent && !pParent->IsAncestorOf(pNewWidget)) {
    if (!pParent->OnKillFocus(pNewWidget)) {
      return false;
    }
  }
  return true;
}

bool CXFA_FFWidget::OnKeyDown(XFA_FWL_VKEYCODE dwKeyCode,
                              Mask<XFA_FWL_KeyFlag> dwFlags) {
  return false;
}

bool CXFA_FFWidget::OnChar(uint32_t dwChar, Mask<XFA_FWL_KeyFlag> dwFlags) {
  return false;
}

FWL_WidgetHit CXFA_FFWidget::HitTest(const CFX_PointF& point) {
  return FWL_WidgetHit::Unknown;
}

bool CXFA_FFWidget::CanUndo() {
  return false;
}

bool CXFA_FFWidget::CanRedo() {
  return false;
}

bool CXFA_FFWidget::CanCopy() {
  return false;
}

bool CXFA_FFWidget::CanCut() {
  return false;
}

bool CXFA_FFWidget::CanPaste() {
  return false;
}

bool CXFA_FFWidget::CanSelectAll() {
  return false;
}

bool CXFA_FFWidget::CanDelete() {
  return CanCut();
}

bool CXFA_FFWidget::CanDeSelect() {
  return CanCopy();
}

bool CXFA_FFWidget::Undo() {
  return false;
}

bool CXFA_FFWidget::Redo() {
  return false;
}

std::optional<WideString> CXFA_FFWidget::Copy() {
  return std::nullopt;
}

std::optional<WideString> CXFA_FFWidget::Cut() {
  return std::nullopt;
}

bool CXFA_FFWidget::Paste(const WideString& wsPaste) {
  return false;
}

void CXFA_FFWidget::SelectAll() {}

void CXFA_FFWidget::Delete() {}

void CXFA_FFWidget::DeSelect() {}

WideString CXFA_FFWidget::GetText() {
  return WideString();
}

FormFieldType CXFA_FFWidget::GetFormFieldType() {
  return FormFieldType::kXFA;
}

CFX_PointF CXFA_FFWidget::Rotate2Normal(const CFX_PointF& point) {
  CFX_Matrix mt = GetRotateMatrix();
  if (mt.IsIdentity()) {
    return point;
  }

  return mt.GetInverse().Transform(point);
}

CFX_Matrix CXFA_FFWidget::GetRotateMatrix() {
  int32_t rotation = node_->GetRotate();
  if (rotation == 0) {
    return CFX_Matrix();
  }

  CFX_RectF rcWidget = GetRectWithoutRotate();
  CFX_Matrix mt;
  switch (rotation) {
    case 90:
      mt.a = 0;
      mt.b = -1;
      mt.c = 1;
      mt.d = 0;
      mt.e = rcWidget.left - rcWidget.top;
      mt.f = rcWidget.left + rcWidget.top;
      break;
    case 180:
      mt.a = -1;
      mt.b = 0;
      mt.c = 0;
      mt.d = -1;
      mt.e = rcWidget.left * 2;
      mt.f = rcWidget.top * 2;
      break;
    case 270:
      mt.a = 0;
      mt.b = 1;
      mt.c = -1;
      mt.d = 0;
      mt.e = rcWidget.left + rcWidget.top;
      mt.f = rcWidget.top - rcWidget.left;
      break;
  }
  return mt;
}

void CXFA_FFWidget::DisplayCaret(bool bVisible, const CFX_RectF* pRtAnchor) {
  GetDoc()->DisplayCaret(this, bVisible, pRtAnchor);
}

void CXFA_FFWidget::GetBorderColorAndThickness(FX_ARGB* cr, float* fWidth) {
  DCHECK(GetNode()->IsWidgetReady());
  CXFA_Border* borderUI = GetNode()->GetUIBorder();
  if (!borderUI) {
    return;
  }

  CXFA_Edge* edge = borderUI->GetEdgeIfExists(0);
  if (!edge) {
    return;
  }

  *cr = edge->GetColor();
  *fWidth = edge->GetThickness();
}

bool CXFA_FFWidget::IsLayoutRectEmpty() {
  CFX_RectF rtLayout = GetRectWithoutRotate();
  return rtLayout.width < 0.1f && rtLayout.height < 0.1f;
}

CXFA_LayoutItem* CXFA_FFWidget::GetParent() {
  CXFA_Node* pParentNode = node_->GetParent();
  if (!pParentNode) {
    return nullptr;
  }

  CXFA_LayoutProcessor* layout = GetDocView()->GetLayoutProcessor();
  return layout->GetLayoutItem(pParentNode);
}

bool CXFA_FFWidget::IsAncestorOf(CXFA_FFWidget* pWidget) {
  if (!pWidget) {
    return false;
  }

  CXFA_Node* pChildNode = pWidget->GetNode();
  while (pChildNode) {
    if (pChildNode == node_) {
      return true;
    }

    pChildNode = pChildNode->GetParent();
  }
  return false;
}

bool CXFA_FFWidget::PtInActiveRect(const CFX_PointF& point) {
  return GetWidgetRect().Contains(point);
}

CXFA_FFDoc* CXFA_FFWidget::GetDoc() {
  return doc_view_->GetDoc();
}

CXFA_FFApp* CXFA_FFWidget::GetApp() {
  return GetDoc()->GetApp();
}

CXFA_FFApp::CallbackIface* CXFA_FFWidget::GetAppProvider() {
  return GetApp()->GetAppProvider();
}

bool CXFA_FFWidget::HasVisibleStatus() const {
  return GetLayoutItem()->TestStatusBits(XFA_WidgetStatus::kVisible);
}

void CXFA_FFWidget::EventKillFocus() {
  CXFA_ContentLayoutItem* pItem = GetLayoutItem();
  if (pItem->TestStatusBits(XFA_WidgetStatus::kAccess)) {
    pItem->ClearStatusBits(XFA_WidgetStatus::kAccess);
    return;
  }
  CXFA_EventParam eParam(XFA_EVENT_Exit);
  node_->ProcessEvent(GetDocView(), XFA_AttributeValue::Exit, &eParam);
}

bool CXFA_FFWidget::IsButtonDown() {
  return GetLayoutItem()->TestStatusBits(XFA_WidgetStatus::kButtonDown);
}

void CXFA_FFWidget::SetButtonDown(bool bSet) {
  CXFA_ContentLayoutItem* pItem = GetLayoutItem();
  if (bSet) {
    pItem->SetStatusBits(XFA_WidgetStatus::kButtonDown);
  } else {
    pItem->ClearStatusBits(XFA_WidgetStatus::kButtonDown);
  }
}
