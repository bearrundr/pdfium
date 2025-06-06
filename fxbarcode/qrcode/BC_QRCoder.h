// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef FXBARCODE_QRCODE_BC_QRCODER_H_
#define FXBARCODE_QRCODE_BC_QRCODER_H_

#include <memory>

#include "core/fxcrt/unowned_ptr.h"

class CBC_QRCoderErrorCorrectionLevel;
class CBC_CommonByteMatrix;

class CBC_QRCoder final {
 public:
  static constexpr int32_t kNumMaskPatterns = 8;

  CBC_QRCoder();
  ~CBC_QRCoder();

  static bool IsValidMaskPattern(int32_t maskPattern);

  const CBC_QRCoderErrorCorrectionLevel* GetECLevel() const;
  int32_t GetVersion() const;
  int32_t GetMatrixWidth() const;
  int32_t GetMaskPattern() const;
  int32_t GetNumTotalBytes() const;
  int32_t GetNumDataBytes() const;
  int32_t GetNumRSBlocks() const;
  std::unique_ptr<CBC_CommonByteMatrix> TakeMatrix();

  bool IsValid() const;

  void SetECLevel(const CBC_QRCoderErrorCorrectionLevel* ecLevel);
  void SetVersion(int32_t version);
  void SetMatrixWidth(int32_t width);
  void SetMaskPattern(int32_t pattern);
  void SetNumDataBytes(int32_t bytes);
  void SetNumTotalBytes(int32_t value);
  void SetNumECBytes(int32_t value);
  void SetNumRSBlocks(int32_t block);
  void SetMatrix(std::unique_ptr<CBC_CommonByteMatrix> pMatrix);

 private:
  UnownedPtr<const CBC_QRCoderErrorCorrectionLevel> ec_level_;
  int32_t version_ = -1;
  int32_t matrix_width_ = -1;
  int32_t mask_pattern_ = -1;
  int32_t num_total_bytes_ = -1;
  int32_t num_data_bytes_ = -1;
  int32_t num_ecbytes_ = -1;
  int32_t num_rsblocks_ = -1;
  std::unique_ptr<CBC_CommonByteMatrix> matrix_;
};

#endif  // FXBARCODE_QRCODE_BC_QRCODER_H_
