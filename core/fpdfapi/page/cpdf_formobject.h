// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef CORE_FPDFAPI_PAGE_CPDF_FORMOBJECT_H_
#define CORE_FPDFAPI_PAGE_CPDF_FORMOBJECT_H_

#include <memory>

#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fxcrt/fx_coordinates.h"

class CPDF_Form;

class CPDF_FormObject final : public CPDF_PageObject {
 public:
  CPDF_FormObject(int32_t content_stream,
                  std::unique_ptr<CPDF_Form> pForm,
                  const CFX_Matrix& matrix);
  ~CPDF_FormObject() override;

  // CPDF_PageObject:
  Type GetType() const override;
  void Transform(const CFX_Matrix& matrix) override;
  bool IsForm() const override;
  CPDF_FormObject* AsForm() override;
  const CPDF_FormObject* AsForm() const override;

  void CalcBoundingBox();
  const CPDF_Form* form() const { return form_.get(); }
  CPDF_Form* form() { return form_.get(); }
  const CFX_Matrix& form_matrix() const { return form_matrix_; }
  void SetFormMatrix(const CFX_Matrix& matrix);

 private:
  std::unique_ptr<CPDF_Form> const form_;
  CFX_Matrix form_matrix_;
};

#endif  // CORE_FPDFAPI_PAGE_CPDF_FORMOBJECT_H_
