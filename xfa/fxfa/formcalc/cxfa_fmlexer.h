// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef XFA_FXFA_FORMCALC_CXFA_FMLEXER_H_
#define XFA_FXFA_FORMCALC_CXFA_FMLEXER_H_

#include "core/fxcrt/raw_span.h"
#include "core/fxcrt/widestring.h"
#include "v8/include/cppgc/macros.h"

enum XFA_FM_TOKEN {
  TOKand,
  TOKlparen,
  TOKrparen,
  TOKmul,
  TOKplus,
  TOKcomma,
  TOKminus,
  TOKdot,
  TOKdiv,
  TOKlt,
  TOKassign,
  TOKgt,
  TOKlbracket,
  TOKrbracket,
  TOKor,
  TOKdotscream,
  TOKdotstar,
  TOKdotdot,
  TOKle,
  TOKne,
  TOKeq,
  TOKge,
  TOKdo,
  TOKkseq,
  TOKksge,
  TOKksgt,
  TOKif,
  TOKin,
  TOKksle,
  TOKkslt,
  TOKksne,
  TOKksor,
  TOKnull,
  TOKbreak,
  TOKksand,
  TOKend,
  TOKeof,
  TOKfor,
  TOKnan,
  TOKksnot,
  TOKvar,
  TOKthen,
  TOKelse,
  TOKexit,
  TOKdownto,
  TOKreturn,
  TOKinfinity,
  TOKendwhile,
  TOKforeach,
  TOKendfunc,
  TOKelseif,
  TOKwhile,
  TOKendfor,
  TOKthrow,
  TOKstep,
  TOKupto,
  TOKcontinue,
  TOKfunc,
  TOKendif,
  TOKstar,
  TOKidentifier,
  TOKunderscore,
  TOKdollar,
  TOKexclamation,
  TOKcall,
  TOKstring,
  TOKnumber,
  TOKreserver
};

class CXFA_FMLexer {
  CPPGC_STACK_ALLOCATED();  // Raw pointers allowed.

 public:
  class Token {
   public:
    Token();
    explicit Token(XFA_FM_TOKEN token);
    Token(XFA_FM_TOKEN token, WideStringView str);
    Token(const Token& that);
    ~Token();

    XFA_FM_TOKEN GetType() const { return type_; }
    WideStringView GetString() const { return string_; }

   private:
    XFA_FM_TOKEN type_ = TOKreserver;
    WideStringView string_;
  };

  explicit CXFA_FMLexer(WideStringView wsFormcalc);
  ~CXFA_FMLexer();

  Token NextToken();
  bool IsComplete() const { return cursor_ >= input_.size(); }

 private:
  Token AdvanceForNumber();
  Token AdvanceForString();
  Token AdvanceForIdentifier();
  void AdvanceForComment();

  void RaiseError() { lexer_error_ = true; }

  pdfium::raw_span<const wchar_t> input_;
  size_t cursor_ = 0;
  bool lexer_error_ = false;
};

#endif  // XFA_FXFA_FORMCALC_CXFA_FMLEXER_H_
