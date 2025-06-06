// Copyright 2016 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fpdfapi/parser/cpdf_syntax_parser.h"

#include <algorithm>
#include <utility>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_crypto_handler.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_null.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_read_validator.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcrt/autorestorer.h"
#include "core/fxcrt/cfx_read_only_vector_stream.h"
#include "core/fxcrt/check.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/fixed_size_data_vector.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/fx_memcpy_wrappers.h"
#include "core/fxcrt/fx_safe_types.h"

namespace {

enum class ReadStatus {
  kNormal,
  kBackslash,
  kOctal,
  kFinishOctal,
  kCarriageReturn
};

class ReadableSubStream final : public IFX_SeekableReadStream {
 public:
  ReadableSubStream(RetainPtr<IFX_SeekableReadStream> pFileRead,
                    FX_FILESIZE part_offset,
                    FX_FILESIZE part_size)
      : file_read_(std::move(pFileRead)),
        part_offset_(part_offset),
        part_size_(part_size) {}

  ~ReadableSubStream() override = default;

  // IFX_SeekableReadStream overrides:
  bool ReadBlockAtOffset(pdfium::span<uint8_t> buffer,
                         FX_FILESIZE offset) override {
    FX_SAFE_FILESIZE safe_end = offset;
    safe_end += buffer.size();
    // Check that requested range is valid, to prevent calling of ReadBlock
    // of original file_read_ with incorrect params.
    if (!safe_end.IsValid() || safe_end.ValueOrDie() > part_size_) {
      return false;
    }

    return file_read_->ReadBlockAtOffset(buffer, part_offset_ + offset);
  }

  FX_FILESIZE GetSize() override { return part_size_; }

 private:
  RetainPtr<IFX_SeekableReadStream> file_read_;
  FX_FILESIZE part_offset_;
  FX_FILESIZE part_size_;
};

}  // namespace

// static
int CPDF_SyntaxParser::s_CurrentRecursionDepth = 0;

// static
std::unique_ptr<CPDF_SyntaxParser> CPDF_SyntaxParser::CreateForTesting(
    RetainPtr<IFX_SeekableReadStream> pFileAccess,
    FX_FILESIZE HeaderOffset) {
  return std::make_unique<CPDF_SyntaxParser>(
      pdfium::MakeRetain<CPDF_ReadValidator>(std::move(pFileAccess), nullptr),
      HeaderOffset);
}

CPDF_SyntaxParser::CPDF_SyntaxParser(
    RetainPtr<IFX_SeekableReadStream> pFileAccess)
    : CPDF_SyntaxParser(
          pdfium::MakeRetain<CPDF_ReadValidator>(std::move(pFileAccess),
                                                 nullptr),
          0) {}

CPDF_SyntaxParser::CPDF_SyntaxParser(RetainPtr<CPDF_ReadValidator> validator,
                                     FX_FILESIZE HeaderOffset)
    : file_access_(std::move(validator)),
      header_offset_(HeaderOffset),
      file_len_(file_access_->GetSize()) {
  DCHECK(header_offset_ <= file_len_);
}

CPDF_SyntaxParser::~CPDF_SyntaxParser() = default;

bool CPDF_SyntaxParser::GetCharAt(FX_FILESIZE pos, uint8_t& ch) {
  AutoRestorer<FX_FILESIZE> save_pos(&pos_);
  pos_ = pos;
  return GetNextChar(ch);
}

bool CPDF_SyntaxParser::ReadBlockAt(FX_FILESIZE read_pos) {
  if (read_pos >= file_len_) {
    return false;
  }
  size_t read_size = read_buffer_size_;
  FX_SAFE_FILESIZE safe_end = read_pos;
  safe_end += read_size;
  if (!safe_end.IsValid() || safe_end.ValueOrDie() > file_len_) {
    read_size = file_len_ - read_pos;
  }

  file_buf_.resize(read_size);
  if (!file_access_->ReadBlockAtOffset(file_buf_, read_pos)) {
    file_buf_.clear();
    return false;
  }

  buf_offset_ = read_pos;
  return true;
}

bool CPDF_SyntaxParser::GetNextChar(uint8_t& ch) {
  FX_FILESIZE pos = pos_ + header_offset_;
  if (pos >= file_len_) {
    return false;
  }

  if (!IsPositionRead(pos) && !ReadBlockAt(pos)) {
    return false;
  }

  ch = file_buf_[pos - buf_offset_];
  pos_++;
  return true;
}

FX_FILESIZE CPDF_SyntaxParser::GetDocumentSize() const {
  return file_len_ - header_offset_;
}

bool CPDF_SyntaxParser::GetCharAtBackward(FX_FILESIZE pos, uint8_t* ch) {
  pos += header_offset_;
  if (pos >= file_len_) {
    return false;
  }

  if (!IsPositionRead(pos)) {
    FX_FILESIZE block_start = 0;
    if (pos >= CPDF_Stream::kFileBufSize) {
      block_start = pos - CPDF_Stream::kFileBufSize + 1;
    }
    if (!ReadBlockAt(block_start) || !IsPositionRead(pos)) {
      return false;
    }
  }
  *ch = file_buf_[pos - buf_offset_];
  return true;
}

bool CPDF_SyntaxParser::ReadBlock(pdfium::span<uint8_t> buffer) {
  if (!file_access_->ReadBlockAtOffset(buffer, pos_ + header_offset_)) {
    return false;
  }
  pos_ += buffer.size();
  return true;
}

CPDF_SyntaxParser::WordType CPDF_SyntaxParser::GetNextWordInternal() {
  word_size_ = 0;
  WordType word_type = WordType::kNumber;

  ToNextWord();
  uint8_t ch;
  if (!GetNextChar(ch)) {
    return word_type;
  }

  if (PDFCharIsDelimiter(ch)) {
    word_type = WordType::kWord;

    word_buffer_[word_size_++] = ch;
    if (ch == '/') {
      while (true) {
        if (!GetNextChar(ch)) {
          return word_type;
        }

        if (!PDFCharIsOther(ch) && !PDFCharIsNumeric(ch)) {
          pos_--;
          return word_type;
        }

        if (word_size_ < sizeof(word_buffer_) - 1) {
          word_buffer_[word_size_++] = ch;
        }
      }
    } else if (ch == '<') {
      if (!GetNextChar(ch)) {
        return word_type;
      }

      if (ch == '<') {
        word_buffer_[word_size_++] = ch;
      } else {
        pos_--;
      }
    } else if (ch == '>') {
      if (!GetNextChar(ch)) {
        return word_type;
      }

      if (ch == '>') {
        word_buffer_[word_size_++] = ch;
      } else {
        pos_--;
      }
    }
    return word_type;
  }

  while (true) {
    if (word_size_ < sizeof(word_buffer_) - 1) {
      word_buffer_[word_size_++] = ch;
    }

    if (!PDFCharIsNumeric(ch)) {
      word_type = WordType::kWord;
    }

    if (!GetNextChar(ch)) {
      return word_type;
    }

    if (PDFCharIsDelimiter(ch) || PDFCharIsWhitespace(ch)) {
      pos_--;
      break;
    }
  }
  return word_type;
}

ByteString CPDF_SyntaxParser::ReadString() {
  uint8_t ch;
  if (!GetNextChar(ch)) {
    return ByteString();
  }

  ByteString buf;
  int32_t parlevel = 0;
  ReadStatus status = ReadStatus::kNormal;
  int32_t iEscCode = 0;
  while (true) {
    switch (status) {
      case ReadStatus::kNormal:
        if (ch == ')') {
          if (parlevel == 0) {
            return ByteString(buf);
          }
          parlevel--;
        } else if (ch == '(') {
          parlevel++;
        }
        if (ch == '\\') {
          status = ReadStatus::kBackslash;
        } else {
          buf += static_cast<char>(ch);
        }
        break;
      case ReadStatus::kBackslash:
        if (FXSYS_IsOctalDigit(ch)) {
          iEscCode = FXSYS_DecimalCharToInt(static_cast<wchar_t>(ch));
          status = ReadStatus::kOctal;
          break;
        }
        if (ch == '\r') {
          status = ReadStatus::kCarriageReturn;
          break;
        }
        if (ch == 'n') {
          buf += '\n';
        } else if (ch == 'r') {
          buf += '\r';
        } else if (ch == 't') {
          buf += '\t';
        } else if (ch == 'b') {
          buf += '\b';
        } else if (ch == 'f') {
          buf += '\f';
        } else if (ch != '\n') {
          buf += static_cast<char>(ch);
        }
        status = ReadStatus::kNormal;
        break;
      case ReadStatus::kOctal:
        if (FXSYS_IsOctalDigit(ch)) {
          iEscCode =
              iEscCode * 8 + FXSYS_DecimalCharToInt(static_cast<wchar_t>(ch));
          status = ReadStatus::kFinishOctal;
        } else {
          buf += static_cast<char>(iEscCode);
          status = ReadStatus::kNormal;
          continue;
        }
        break;
      case ReadStatus::kFinishOctal:
        status = ReadStatus::kNormal;
        if (FXSYS_IsOctalDigit(ch)) {
          iEscCode =
              iEscCode * 8 + FXSYS_DecimalCharToInt(static_cast<wchar_t>(ch));
          buf += static_cast<char>(iEscCode);
        } else {
          buf += static_cast<char>(iEscCode);
          continue;
        }
        break;
      case ReadStatus::kCarriageReturn:
        status = ReadStatus::kNormal;
        if (ch != '\n') {
          continue;
        }
        break;
    }

    if (!GetNextChar(ch)) {
      break;
    }
  }

  GetNextChar(ch);
  return buf;
}

DataVector<uint8_t> CPDF_SyntaxParser::ReadHexString() {
  uint8_t ch;
  if (!GetNextChar(ch)) {
    return DataVector<uint8_t>();
  }

  DataVector<uint8_t> buf;
  bool bFirst = true;
  uint8_t code = 0;
  while (true) {
    if (ch == '>') {
      break;
    }

    if (FXSYS_IsHexDigit(ch)) {
      int val = FXSYS_HexCharToInt(ch);
      if (bFirst) {
        code = val * 16;
      } else {
        code += val;
        buf.push_back(code);
      }
      bFirst = !bFirst;
    }

    if (!GetNextChar(ch)) {
      break;
    }
  }
  if (!bFirst) {
    buf.push_back(code);
  }

  return buf;
}

void CPDF_SyntaxParser::ToNextLine() {
  uint8_t ch;
  while (GetNextChar(ch)) {
    if (ch == '\n') {
      break;
    }

    if (ch == '\r') {
      GetNextChar(ch);
      if (ch != '\n') {
        --pos_;
      }
      break;
    }
  }
}

void CPDF_SyntaxParser::ToNextWord() {
  if (trailer_ends_) {
    RecordingToNextWord();
    return;
  }

  uint8_t ch;
  if (!GetNextChar(ch)) {
    return;
  }

  while (true) {
    while (PDFCharIsWhitespace(ch)) {
      if (!GetNextChar(ch)) {
        return;
      }
    }

    if (ch != '%') {
      break;
    }

    while (true) {
      if (!GetNextChar(ch)) {
        return;
      }
      if (PDFCharIsLineEnding(ch)) {
        break;
      }
    }
  }
  pos_--;
}

// A state machine which goes % -> E -> O -> F -> line ending.
enum class EofState {
  kInitial = 0,
  kNonPercent,
  kPercent,
  kE,
  kO,
  kF,
  kInvalid,
};

void CPDF_SyntaxParser::RecordingToNextWord() {
  DCHECK(trailer_ends_);

  EofState eof_state = EofState::kInitial;
  // Find the first character which is neither whitespace, nor part of a
  // comment.
  while (true) {
    uint8_t ch;
    if (!GetNextChar(ch)) {
      return;
    }
    switch (eof_state) {
      case EofState::kInitial:
        if (!PDFCharIsWhitespace(ch)) {
          eof_state = ch == '%' ? EofState::kPercent : EofState::kNonPercent;
        }
        break;
      case EofState::kNonPercent:
        break;
      case EofState::kPercent:
        if (ch == 'E') {
          eof_state = EofState::kE;
        } else if (ch != '%') {
          eof_state = EofState::kInvalid;
        }
        break;
      case EofState::kE:
        eof_state = ch == 'O' ? EofState::kO : EofState::kInvalid;
        break;
      case EofState::kO:
        eof_state = ch == 'F' ? EofState::kF : EofState::kInvalid;
        break;
      case EofState::kF:
        if (ch == '\r') {
          // See if \r has to be combined with a \n that follows it
          // immediately.
          if (GetNextChar(ch) && ch != '\n') {
            ch = '\r';
            pos_--;
          }
        }
        // If we now have a \r, that's not followed by a \n, so both are OK.
        if (ch == '\r' || ch == '\n') {
          trailer_ends_->push_back(pos_);
        }
        eof_state = EofState::kInvalid;
        break;
      case EofState::kInvalid:
        break;
    }
    if (PDFCharIsLineEnding(ch)) {
      eof_state = EofState::kInitial;
    }
    if (eof_state == EofState::kNonPercent) {
      break;
    }
  }
  pos_--;
}

CPDF_SyntaxParser::WordResult CPDF_SyntaxParser::GetNextWord() {
  CPDF_ReadValidator::ScopedSession read_session(GetValidator());
  WordType word_type = GetNextWordInternal();
  ByteStringView word;
  if (!GetValidator()->has_read_problems()) {
    word = ByteStringView(pdfium::span(word_buffer_).first(word_size_));
  }
  return {ByteString(word), word_type == WordType::kNumber};
}

ByteString CPDF_SyntaxParser::PeekNextWord() {
  AutoRestorer<FX_FILESIZE> save_pos(&pos_);
  return GetNextWord().word;
}

ByteString CPDF_SyntaxParser::GetKeyword() {
  return GetNextWord().word;
}

void CPDF_SyntaxParser::SetPos(FX_FILESIZE pos) {
  DCHECK_GE(pos, 0);
  pos_ = std::min(pos, file_len_);
}

RetainPtr<CPDF_Object> CPDF_SyntaxParser::GetObjectBody(
    CPDF_IndirectObjectHolder* pObjList) {
  CPDF_ReadValidator::ScopedSession read_session(GetValidator());
  auto result = GetObjectBodyInternal(pObjList, ParseType::kLoose);
  if (GetValidator()->has_read_problems()) {
    return nullptr;
  }
  return result;
}

RetainPtr<CPDF_Object> CPDF_SyntaxParser::GetObjectBodyInternal(
    CPDF_IndirectObjectHolder* pObjList,
    ParseType parse_type) {
  AutoRestorer<int> depth_restorer(&s_CurrentRecursionDepth);
  if (++s_CurrentRecursionDepth > kParserMaxRecursionDepth) {
    return nullptr;
  }

  FX_FILESIZE SavedObjPos = pos_;
  WordResult word_result = GetNextWord();
  const ByteString& word = word_result.word;
  if (word.IsEmpty()) {
    return nullptr;
  }

  if (word_result.is_number) {
    AutoRestorer<FX_FILESIZE> pos_restorer(&pos_);
    WordResult nextword = GetNextWord();
    if (!nextword.is_number) {
      return pdfium::MakeRetain<CPDF_Number>(word.AsStringView());
    }

    WordResult nextword2 = GetNextWord();
    if (nextword2.word != "R") {
      return pdfium::MakeRetain<CPDF_Number>(word.AsStringView());
    }

    pos_restorer.AbandonRestoration();
    uint32_t refnum = FXSYS_atoui(word.c_str());
    if (refnum == CPDF_Object::kInvalidObjNum) {
      return nullptr;
    }

    return pdfium::MakeRetain<CPDF_Reference>(pObjList, refnum);
  }

  if (word == "true" || word == "false") {
    return pdfium::MakeRetain<CPDF_Boolean>(word == "true");
  }

  if (word == "null") {
    return pdfium::MakeRetain<CPDF_Null>();
  }

  if (word == "(") {
    return pdfium::MakeRetain<CPDF_String>(pool_, ReadString());
  }
  if (word == "<") {
    return pdfium::MakeRetain<CPDF_String>(pool_, ReadHexString(),
                                           CPDF_String::DataType::kIsHex);
  }
  if (word == "[") {
    auto pArray = pdfium::MakeRetain<CPDF_Array>();
    while (RetainPtr<CPDF_Object> pObj =
               GetObjectBodyInternal(pObjList, ParseType::kLoose)) {
      // `pObj` cannot be a stream, per ISO 32000-1:2008 section 7.3.8.1.
      if (!pObj->IsStream()) {
        pArray->Append(std::move(pObj));
      }
    }
    return (parse_type == ParseType::kLoose || word_buffer_[0] == ']')
               ? std::move(pArray)
               : nullptr;
  }
  if (word[0] == '/') {
    auto word_span = pdfium::span(word_buffer_).first(word_size_);
    return pdfium::MakeRetain<CPDF_Name>(
        pool_, PDF_NameDecode(ByteStringView(word_span).Substr(1)));
  }
  if (word == "<<") {
    RetainPtr<CPDF_Dictionary> pDict =
        pdfium::MakeRetain<CPDF_Dictionary>(pool_);
    while (true) {
      WordResult inner_word_result = GetNextWord();
      const ByteString& inner_word = inner_word_result.word;
      if (inner_word.IsEmpty()) {
        return nullptr;
      }

      FX_FILESIZE SavedPos = pos_ - inner_word.GetLength();
      if (inner_word == ">>") {
        break;
      }

      if (inner_word == "endobj") {
        pos_ = SavedPos;
        break;
      }
      if (inner_word[0] != '/') {
        continue;
      }

      ByteString key = PDF_NameDecode(inner_word.AsStringView());
      if (key.IsEmpty() && parse_type == ParseType::kLoose) {
        continue;
      }

      RetainPtr<CPDF_Object> pObj =
          GetObjectBodyInternal(pObjList, ParseType::kLoose);
      if (!pObj) {
        if (parse_type == ParseType::kLoose) {
          continue;
        }

        ToNextLine();
        return nullptr;
      }

      // `key` has to be "/X" at the minimum.
      // `pObj` cannot be a stream, per ISO 32000-1:2008 section 7.3.8.1.
      if (key.GetLength() > 1 && !pObj->IsStream()) {
        pDict->SetFor(key.Substr(1), std::move(pObj));
      }
    }

    AutoRestorer<FX_FILESIZE> pos_restorer(&pos_);
    if (GetNextWord().word != "stream") {
      return pDict;
    }
    pos_restorer.AbandonRestoration();
    return ReadStream(std::move(pDict));
  }
  if (word == ">>") {
    pos_ = SavedObjPos;
  }

  return nullptr;
}

RetainPtr<CPDF_Object> CPDF_SyntaxParser::GetIndirectObject(
    CPDF_IndirectObjectHolder* pObjList,
    ParseType parse_type) {
  CPDF_ReadValidator::ScopedSession read_session(GetValidator());
  const FX_FILESIZE saved_pos = GetPos();

  WordResult objnum_word_result = GetNextWord();
  if (!objnum_word_result.is_number || objnum_word_result.word.IsEmpty()) {
    SetPos(saved_pos);
    return nullptr;
  }
  const uint32_t parser_objnum = FXSYS_atoui(objnum_word_result.word.c_str());

  WordResult gennum_word_result = GetNextWord();
  const ByteString& gennum_word = gennum_word_result.word;
  if (!gennum_word_result.is_number || gennum_word.IsEmpty()) {
    SetPos(saved_pos);
    return nullptr;
  }
  const uint32_t parser_gennum = FXSYS_atoui(gennum_word.c_str());

  if (GetKeyword() != "obj") {
    SetPos(saved_pos);
    return nullptr;
  }

  RetainPtr<CPDF_Object> pObj = GetObjectBodyInternal(pObjList, parse_type);
  if (pObj) {
    pObj->SetObjNum(parser_objnum);
    pObj->SetGenNum(parser_gennum);
  }

  return GetValidator()->has_read_problems() ? nullptr : pObj;
}

unsigned int CPDF_SyntaxParser::ReadEOLMarkers(FX_FILESIZE pos) {
  unsigned char byte1 = 0;
  unsigned char byte2 = 0;

  GetCharAt(pos, byte1);
  GetCharAt(pos + 1, byte2);

  if (byte1 == '\r' && byte2 == '\n') {
    return 2;
  }

  if (byte1 == '\r' || byte1 == '\n') {
    return 1;
  }

  return 0;
}

FX_FILESIZE CPDF_SyntaxParser::FindWordPos(ByteStringView word) {
  AutoRestorer<FX_FILESIZE> pos_restorer(&pos_);
  FX_FILESIZE end_offset = FindTag(word);
  while (end_offset >= 0) {
    // Stop searching when word is found.
    if (IsWholeWord(GetPos() - word.GetLength(), file_len_, word, true)) {
      return GetPos() - word.GetLength();
    }

    end_offset = FindTag(word);
  }
  return -1;
}

FX_FILESIZE CPDF_SyntaxParser::FindStreamEndPos() {
  const ByteStringView kEndStreamStr("endstream");
  const ByteStringView kEndObjStr("endobj");

  FX_FILESIZE endStreamWordOffset = FindWordPos(kEndStreamStr);
  FX_FILESIZE endObjWordOffset = FindWordPos(kEndObjStr);

  // Can't find "endstream" or "endobj".
  if (endStreamWordOffset < 0 && endObjWordOffset < 0) {
    return -1;
  }

  if (endStreamWordOffset < 0 && endObjWordOffset >= 0) {
    // Correct the position of end stream.
    endStreamWordOffset = endObjWordOffset;
  } else if (endStreamWordOffset >= 0 && endObjWordOffset < 0) {
    // Correct the position of end obj.
    endObjWordOffset = endStreamWordOffset;
  } else if (endStreamWordOffset > endObjWordOffset) {
    endStreamWordOffset = endObjWordOffset;
  }

  int numMarkers = ReadEOLMarkers(endStreamWordOffset - 2);
  if (numMarkers == 2) {
    endStreamWordOffset -= 2;
  } else {
    numMarkers = ReadEOLMarkers(endStreamWordOffset - 1);
    if (numMarkers == 1) {
      endStreamWordOffset -= 1;
    }
  }
  if (endStreamWordOffset < GetPos()) {
    return -1;
  }
  return endStreamWordOffset;
}

RetainPtr<CPDF_Stream> CPDF_SyntaxParser::ReadStream(
    RetainPtr<CPDF_Dictionary> pDict) {
  RetainPtr<const CPDF_Number> pLenObj =
      ToNumber(pDict->GetDirectObjectFor("Length"));
  FX_FILESIZE len = pLenObj ? pLenObj->GetInteger() : -1;

  // Locate the start of stream.
  ToNextLine();
  const FX_FILESIZE streamStartPos = GetPos();

  if (len > 0) {
    FX_SAFE_FILESIZE pos = GetPos();
    pos += len;
    if (!pos.IsValid() || pos.ValueOrDie() >= file_len_) {
      len = -1;
    }
  }

  RetainPtr<IFX_SeekableReadStream> substream;
  if (len > 0) {
    // Check data availability first to allow the Validator to request data
    // smoothly, without jumps.
    if (!GetValidator()->CheckDataRangeAndRequestIfUnavailable(
            header_offset_ + GetPos(), len)) {
      return nullptr;
    }

    substream = pdfium::MakeRetain<ReadableSubStream>(
        GetValidator(), header_offset_ + GetPos(), len);
    SetPos(GetPos() + len);
  }

  const ByteStringView kEndStreamStr("endstream");
  const ByteStringView kEndObjStr("endobj");

  // Note, we allow zero length streams as we need to pass them through when we
  // are importing pages into a new document.
  if (len >= 0) {
    CPDF_ReadValidator::ScopedSession read_session(GetValidator());
    pos_ += ReadEOLMarkers(GetPos());
    const size_t zap_length = kEndStreamStr.GetLength() + 1;
    std::ranges::fill(pdfium::span(word_buffer_).first(zap_length), 0);
    GetNextWordInternal();
    if (GetValidator()->has_read_problems()) {
      return nullptr;
    }

    // Earlier version of PDF specification doesn't require EOL marker before
    // 'endstream' keyword. If keyword 'endstream' follows the bytes in
    // specified length, it signals the end of stream.
    if (UNSAFE_TODO(memcmp(word_buffer_.data(),
                           kEndStreamStr.unterminated_unsigned_str(),
                           kEndStreamStr.GetLength())) != 0) {
      substream.Reset();
      len = -1;
      SetPos(streamStartPos);
    }
  }

  if (len < 0) {
    // If len is not available or incorrect, len needs to be calculated
    // by searching the keywords "endstream" or "endobj".
    const FX_FILESIZE streamEndPos = FindStreamEndPos();
    if (streamEndPos < 0) {
      return nullptr;
    }

    len = streamEndPos - streamStartPos;
    DCHECK_GE(len, 0);
    if (len > 0) {
      SetPos(streamStartPos);
      // Check data availability first to allow the Validator to request data
      // smoothly, without jumps.
      if (!GetValidator()->CheckDataRangeAndRequestIfUnavailable(
              header_offset_ + GetPos(), len)) {
        return nullptr;
      }

      substream = pdfium::MakeRetain<ReadableSubStream>(
          GetValidator(), header_offset_ + GetPos(), len);
      SetPos(GetPos() + len);
    }
  }

  RetainPtr<CPDF_Stream> stream;
  if (substream) {
    // It is unclear from CPDF_SyntaxParser's perspective what object
    // `substream` is ultimately holding references to. To avoid unexpectedly
    // changing object lifetimes by handing `substream` to `stream`, make a
    // copy of the data here.
    auto data = FixedSizeDataVector<uint8_t>::Uninit(substream->GetSize());
    bool did_read = substream->ReadBlockAtOffset(data.span(), 0);
    CHECK(did_read);
    auto data_as_stream =
        pdfium::MakeRetain<CFX_ReadOnlyVectorStream>(std::move(data));

    stream = pdfium::MakeRetain<CPDF_Stream>(std::move(data_as_stream),
                                             std::move(pDict));
  } else {
    DCHECK(!len);
    stream = pdfium::MakeRetain<CPDF_Stream>(std::move(pDict));
  }
  const FX_FILESIZE end_stream_offset = GetPos();
  const size_t zap_length = kEndObjStr.GetLength() + 1;
  std::ranges::fill(pdfium::span(word_buffer_).first(zap_length), 0);
  GetNextWordInternal();

  // Allow whitespace after endstream and before a newline.
  unsigned char ch = 0;
  while (GetNextChar(ch)) {
    if (!PDFCharIsWhitespace(ch) || PDFCharIsLineEnding(ch)) {
      break;
    }
  }
  SetPos(GetPos() - 1);

  int numMarkers = ReadEOLMarkers(GetPos());
  if (word_size_ == static_cast<unsigned int>(kEndObjStr.GetLength()) &&
      numMarkers != 0 &&
      UNSAFE_TODO(memcmp(word_buffer_.data(),
                         kEndObjStr.unterminated_unsigned_str(),
                         kEndObjStr.GetLength())) == 0) {
    SetPos(end_stream_offset);
  }
  return stream;
}

uint32_t CPDF_SyntaxParser::GetDirectNum() {
  if (GetNextWordInternal() != WordType::kNumber) {
    return 0;
  }

  word_buffer_[word_size_] = 0;
  return FXSYS_atoui(pdfium::as_chars(pdfium::span(word_buffer_)).data());
}

RetainPtr<CPDF_ReadValidator> CPDF_SyntaxParser::GetValidator() const {
  return file_access_;
}

bool CPDF_SyntaxParser::IsWholeWord(FX_FILESIZE startpos,
                                    FX_FILESIZE limit,
                                    ByteStringView tag,
                                    bool checkKeyword) {
  const uint32_t taglen = tag.GetLength();

  bool bCheckLeft = !PDFCharIsDelimiter(tag[0]) && !PDFCharIsWhitespace(tag[0]);
  bool bCheckRight = !PDFCharIsDelimiter(tag[taglen - 1]) &&
                     !PDFCharIsWhitespace(tag[taglen - 1]);

  uint8_t ch;
  if (bCheckRight && startpos + static_cast<int32_t>(taglen) <= limit &&
      GetCharAt(startpos + static_cast<int32_t>(taglen), ch)) {
    if (PDFCharIsNumeric(ch) || PDFCharIsOther(ch) ||
        (checkKeyword && PDFCharIsDelimiter(ch))) {
      return false;
    }
  }

  if (bCheckLeft && startpos > 0 && GetCharAt(startpos - 1, ch)) {
    if (PDFCharIsNumeric(ch) || PDFCharIsOther(ch) ||
        (checkKeyword && PDFCharIsDelimiter(ch))) {
      return false;
    }
  }
  return true;
}

bool CPDF_SyntaxParser::BackwardsSearchToWord(ByteStringView word,
                                              FX_FILESIZE limit) {
  int32_t taglen = word.GetLength();
  if (taglen == 0) {
    return false;
  }

  FX_FILESIZE pos = pos_;
  int32_t offset = taglen - 1;
  while (true) {
    if (limit && pos <= pos_ - limit) {
      return false;
    }

    uint8_t byte;
    if (!GetCharAtBackward(pos, &byte)) {
      return false;
    }

    if (byte == word[offset]) {
      offset--;
      if (offset >= 0) {
        pos--;
        continue;
      }
      if (IsWholeWord(pos, limit, word, false)) {
        pos_ = pos;
        return true;
      }
    }
    offset = byte == word[taglen - 1] ? taglen - 2 : taglen - 1;
    pos--;
    if (pos < 0) {
      return false;
    }
  }
}

FX_FILESIZE CPDF_SyntaxParser::FindTag(ByteStringView tag) {
  const FX_FILESIZE startpos = GetPos();
  const int32_t taglen = tag.GetLength();
  DCHECK_GT(taglen, 0);

  while (true) {
    const FX_FILESIZE match_start_pos = GetPos();
    bool match_found = true;

    for (int32_t i = 0; i < taglen; i++) {
      uint8_t ch;
      if (!GetNextChar(ch)) {
        return -1;
      }

      if (ch != tag[i]) {
        match_found = false;
        break;
      }
    }

    if (match_found) {
      return match_start_pos - startpos;
    }

    // On running into a mismatch, the code restarts from one place ahead w.r.t.
    // the previous iteration.
    SetPos(match_start_pos + 1);
  }
}

bool CPDF_SyntaxParser::IsPositionRead(FX_FILESIZE pos) const {
  return buf_offset_ <= pos &&
         pos < static_cast<FX_FILESIZE>(buf_offset_ + file_buf_.size());
}
