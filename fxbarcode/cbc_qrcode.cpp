// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com
/*
 * Copyright 2011 ZXing authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fxbarcode/cbc_qrcode.h"

#include <stdint.h>

#include <memory>

#include "core/fxcrt/data_vector.h"
#include "fxbarcode/qrcode/BC_QRCodeWriter.h"

CBC_QRCode::CBC_QRCode() : CBC_CodeBase(std::make_unique<CBC_QRCodeWriter>()) {}

CBC_QRCode::~CBC_QRCode() = default;

bool CBC_QRCode::Encode(WideStringView contents) {
  int32_t width;
  int32_t height;
  CBC_QRCodeWriter* pWriter = GetQRCodeWriter();
  DataVector<uint8_t> data = pWriter->Encode(
      contents, pWriter->error_correction_level(), &width, &height);
  return pWriter->RenderResult(data, width, height);
}

bool CBC_QRCode::RenderDevice(CFX_RenderDevice* device,
                              const CFX_Matrix& matrix) {
  GetQRCodeWriter()->RenderDeviceResult(device, matrix);
  return true;
}

BC_TYPE CBC_QRCode::GetType() {
  return BC_TYPE::kQRCode;
}

CBC_QRCodeWriter* CBC_QRCode::GetQRCodeWriter() {
  return static_cast<CBC_QRCodeWriter*>(bc_writer_.get());
}
