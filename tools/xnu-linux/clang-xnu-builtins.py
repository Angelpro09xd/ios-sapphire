#!/usr/bin/env python3
# clang-xnu-builtins.py CLANG_SRC: what Apple's clang has for building xnu-8796, added to an LLVM 18 clang tree.
#
# kalloc_type (osfmk/kern/kalloc.h, libkern/c++/OSData.h, OSAllocation.h) asks the compiler about a type's
# memory layout in 8-byte granules:
#   __builtin_xnu_type_summary(T)        bit (1 << g) set for each granule encoding g in T
#   __builtin_xnu_type_signature(T)      a string with one character per granule
#   __builtin_xnu_types_compatible(A, B) A and B are the same type or have the same signature
# A granule's encoding ORs KT_GRANULE_POINTER (1) for pointers and KT_GRANULE_DATA (2) for other scalars
# (padding is 0). __attribute__((xnu_usage_semantics("pointer"|"data", ...))) on a typedef or a field
# (sys/cdefs.h __kernel_ptr_semantics: ppnum_t, vm_offset_t...) overrides what the type looks like; the
# kernel's static checks rely on it (vm_pageout.c: struct upl_page_info, with a ppnum_t, is not data-only).
# The kernel only compares and hashes the signature strings (kalloc.c), so their spelling here (a hex digit
# per granule) need not be Apple's, only consistent.
#
# Also as in Apple's clang: __typeof__(*p) of a void *p in C++ (kalloc.h KALLOC_TYPE_IS_COMPATIBLE_PTR) is
# fine where it is not evaluated, and -fapple-kext code does not register global destructors with atexit.
import sys
from pathlib import Path

src = Path(sys.argv[1])


def edit(rel, old, new, count=1):
    p = src / rel
    t = p.read_text()
    if new in t:
        return
    if t.count(old) != count:
        raise SystemExit(f'{rel}: expected {count} of {old!r}')
    p.write_text(t.replace(old, new))


edit('include/clang/Basic/TokenKinds.def',
     'ARRAY_TYPE_TRAIT(__array_extent, ArrayExtent, KEYCXX)\n',
     'ARRAY_TYPE_TRAIT(__array_extent, ArrayExtent, KEYCXX)\n'
     '\n'
     '// Apple XNU type layout builtins (kalloc_type)\n'
     'ARRAY_TYPE_TRAIT(__builtin_xnu_type_summary, XnuTypeSummary, KEYALL)\n'
     'ARRAY_TYPE_TRAIT(__builtin_xnu_type_signature, XnuTypeSignature, KEYALL)\n'
     'TYPE_TRAIT_2(__builtin_xnu_types_compatible, XnuTypesCompatible, KEYALL)\n')

edit('include/clang/Basic/Attr.td',
     'def AbiTag : Attr {\n',
     'def XnuUsageSemantics : InheritableAttr {\n'
     '  let Spellings = [GNU<"xnu_usage_semantics">];\n'
     '  let Args = [VariadicStringArgument<"Semantics">];\n'
     '  let Subjects = SubjectList<[TypedefName, Field], ErrorDiag>;\n'
     '  let Documentation = [Undocumented];\n'
     '}\n'
     '\n'
     'def AbiTag : Attr {\n')

edit('lib/Sema/SemaDeclAttr.cpp',
     'static void handleAbiTagAttr(Sema &S, Decl *D, const ParsedAttr &AL) {\n',
     '''static void handleXnuUsageSemanticsAttr(Sema &S, Decl *D,
                                        const ParsedAttr &AL) {
  SmallVector<StringRef, 2> Semantics;
  for (unsigned I = 0, E = AL.getNumArgs(); I != E; ++I) {
    StringRef Str;
    if (!S.checkStringLiteralArgumentAttr(AL, I, Str))
      return;
    Semantics.push_back(Str);
  }
  D->addAttr(::new (S.Context) XnuUsageSemanticsAttr(
      S.Context, AL, Semantics.data(), Semantics.size()));
}

static void handleAbiTagAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
''')

edit('lib/Sema/SemaDeclAttr.cpp',
     '  case ParsedAttr::AT_AbiTag:\n    handleAbiTagAttr(S, D, AL);\n    break;\n',
     '  case ParsedAttr::AT_AbiTag:\n    handleAbiTagAttr(S, D, AL);\n    break;\n'
     '  case ParsedAttr::AT_XnuUsageSemantics:\n    handleXnuUsageSemanticsAttr(S, D, AL);\n    break;\n')

edit('lib/Sema/SemaExpr.cpp',
     '''    if (LO.CPlusPlus)
      S.Diag(OpLoc, diag::err_typecheck_indirection_through_void_pointer_cpp)''',
     '''    if (LO.CPlusPlus && !S.isUnevaluatedContext())
      S.Diag(OpLoc, diag::err_typecheck_indirection_through_void_pointer_cpp)''')

edit('lib/Parse/ParseExpr.cpp',
     '  case tok::kw___array_rank:\n  case tok::kw___array_extent:\n',
     '  case tok::kw___array_rank:\n  case tok::kw___array_extent:\n'
     '  case tok::kw___builtin_xnu_type_summary:\n  case tok::kw___builtin_xnu_type_signature:\n')

edit('lib/Parse/ParseExprCXX.cpp',
     '  switch (ATT) {\n  case ATT_ArrayRank: {\n',
     '  switch (ATT) {\n  case ATT_XnuTypeSummary:\n  case ATT_XnuTypeSignature:\n  case ATT_ArrayRank: {\n')

sema = 'lib/Sema/SemaExprCXX.cpp'

edit(sema,
     '#include "clang/AST/RecursiveASTVisitor.h"\n',
     '#include "clang/AST/RecordLayout.h"\n#include "clang/AST/RecursiveASTVisitor.h"\n')

# before the binary type traits' definition (not its forward declaration), which uses the signatures
edit(sema,
     'static bool EvaluateBinaryTypeTrait(Sema &Self, TypeTrait BTT, QualType LhsT,\n'
     '                                    QualType RhsT, SourceLocation KeyLoc) {\n',
     r'''// XNU type layout (kalloc_type): the encoding of each 8-byte granule of a type.
enum : uint8_t { XnuGranulePointer = 1, XnuGranuleData = 2 };

static void XnuMarkRange(SmallVectorImpl<uint8_t> &G, uint64_t Off,
                         uint64_t Len, uint8_t Enc) {
  if (!Len)
    return;
  for (uint64_t I = Off / 8, E = (Off + Len - 1) / 8; I <= E && I < G.size();
       ++I)
    G[I] |= Enc;
}

// The granule encoding xnu_usage_semantics gives a typedef or field, or 0.
static uint8_t XnuUsageSemantics(const Decl *D) {
  uint8_t Enc = 0;
  if (const auto *A = D->getAttr<XnuUsageSemanticsAttr>())
    for (StringRef S : A->semantics())
      Enc |= S == "pointer" ? XnuGranulePointer
                            : (S == "data" ? XnuGranuleData : 0);
  return Enc;
}

static void XnuMarkType(ASTContext &C, QualType T, uint64_t Off,
                        SmallVectorImpl<uint8_t> &G) {
  if (Off / 8 >= G.size() || T->isIncompleteType() || T->isDependentType() ||
      T->isFunctionType() || T->isSizelessType())
    return;
  uint64_t Size = C.getTypeSizeInChars(T).getQuantity();

  // an annotated typedef decides for the whole value (ppnum_t: a pointer)
  for (QualType S = T;;) {
    const auto *TT = S->getAs<TypedefType>();
    if (!TT)
      break;
    if (uint8_t Enc = XnuUsageSemantics(TT->getDecl())) {
      XnuMarkRange(G, Off, Size, Enc);
      return;
    }
    S = TT->getDecl()->getUnderlyingType();
  }

  // arrays before canonicalizing: the element type keeps its typedefs
  if (const ConstantArrayType *CAT = C.getAsConstantArrayType(T)) {
    QualType E = CAT->getElementType();
    uint64_t N = CAT->getSize().getZExtValue();
    if (E->isIncompleteType() || E->isDependentType())
      return;
    uint64_t ES = C.getTypeSizeInChars(E).getQuantity();
    if (!N || !ES)
      return;
    if (ES % 8 == 0 && Off % 8 == 0) {
      // one element's granules, repeated
      SmallVector<uint8_t, 16> EG(ES / 8, 0);
      XnuMarkType(C, E, 0, EG);
      for (uint64_t I = 0; I < N && (Off + I * ES) / 8 < G.size(); ++I)
        for (uint64_t K = 0; K < EG.size(); ++K)
          if ((Off + I * ES) / 8 + K < G.size())
            G[(Off + I * ES) / 8 + K] |= EG[K];
      return;
    }
    for (uint64_t I = 0; I < N && (Off + I * ES) / 8 < G.size(); ++I)
      XnuMarkType(C, E, Off + I * ES, G);
    return;
  }

  if (const auto *AT = T->getAs<AtomicType>()) {
    XnuMarkType(C, AT->getValueType(), Off, G);
    return;
  }
  QualType CT = C.getCanonicalType(T).getUnqualifiedType();
  if (CT->isArrayType())
    return;

  if (const RecordType *RT = CT->getAs<RecordType>()) {
    const RecordDecl *RD = RT->getDecl()->getDefinition();
    if (!RD || RD->isInvalidDecl())
      return;
    const ASTRecordLayout &L = C.getASTRecordLayout(RD);
    if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD)) {
      // Itanium: a dynamic class without a primary base starts with its vptr
      if (CRD->isDynamicClass() && !L.getPrimaryBase())
        XnuMarkRange(G, Off, C.getTypeSizeInChars(C.VoidPtrTy).getQuantity(),
                     XnuGranulePointer);
      for (const CXXBaseSpecifier &B : CRD->bases()) {
        const CXXRecordDecl *BD = B.getType()->getAsCXXRecordDecl();
        if (!BD || B.isVirtual())
          continue;
        XnuMarkType(C, B.getType(), Off + L.getBaseClassOffset(BD).getQuantity(),
                    G);
      }
      for (const CXXBaseSpecifier &B : CRD->vbases()) {
        const CXXRecordDecl *BD = B.getType()->getAsCXXRecordDecl();
        if (BD)
          XnuMarkType(C, B.getType(),
                      Off + L.getVBaseClassOffset(BD).getQuantity(), G);
      }
    }
    unsigned I = 0;
    for (const FieldDecl *F : RD->fields()) {
      uint64_t Bits = L.getFieldOffset(I++);
      uint8_t Enc = XnuUsageSemantics(F);
      if (F->isBitField()) {
        unsigned W = F->getBitWidthValue(C);
        if (W)
          XnuMarkRange(G, Off + Bits / 8, (Bits % 8 + W + 7) / 8,
                       Enc ? Enc : XnuGranuleData);
        continue;
      }
      if (F->isZeroSize(C))
        continue;
      if (Enc) {
        if (!F->getType()->isIncompleteType())
          XnuMarkRange(G, Off + Bits / 8,
                       C.getTypeSizeInChars(F->getType()).getQuantity(), Enc);
        continue;
      }
      XnuMarkType(C, F->getType(), Off + Bits / 8, G);
    }
    return;
  }

  if (CT->isMemberFunctionPointerType()) {
    XnuMarkRange(G, Off, 8, XnuGranulePointer);
    XnuMarkRange(G, Off + 8, Size - 8, XnuGranuleData);
    return;
  }
  if (CT->isPointerType() || CT->isBlockPointerType() ||
      CT->isReferenceType() || CT->isObjCObjectPointerType()) {
    XnuMarkRange(G, Off, Size, XnuGranulePointer);
    return;
  }
  XnuMarkRange(G, Off, Size, XnuGranuleData);
}

// The granules of T (empty for void, functions and incomplete types). Types
// past 8 MiB are cut there: the kernel never looks at those signatures.
static bool XnuTypeGranules(Sema &S, QualType T, SourceLocation Loc,
                            SmallVectorImpl<uint8_t> &G) {
  ASTContext &C = S.Context;
  T = T.getNonReferenceType();
  if (T->isVoidType() || T->isFunctionType() || T->isSizelessType() ||
      !S.isCompleteType(Loc, T))
    return false;
  uint64_t Size = C.getTypeSizeInChars(T).getQuantity();
  G.assign(std::min<uint64_t>((Size + 7) / 8, 1u << 20), 0);
  XnuMarkType(C, T, 0, G);
  return true;
}

static uint64_t XnuTypeSummary(Sema &S, QualType T, SourceLocation Loc) {
  SmallVector<uint8_t, 64> G;
  if (!XnuTypeGranules(S, T, Loc, G))
    return 0;
  uint64_t Summary = 0;
  for (uint8_t E : G)
    Summary |= uint64_t(1) << E;
  return Summary;
}

static std::string XnuTypeSignature(Sema &S, QualType T, SourceLocation Loc) {
  SmallVector<uint8_t, 64> G;
  std::string Sig;
  if (!XnuTypeGranules(S, T, Loc, G))
    return Sig;
  Sig.reserve(G.size());
  for (uint8_t E : G)
    Sig.push_back("0123456789abcdef"[E & 15]);
  return Sig;
}

static bool EvaluateBinaryTypeTrait(Sema &Self, TypeTrait BTT, QualType LhsT,
                                    QualType RhsT, SourceLocation KeyLoc) {
''')

edit(sema,
     '''  switch(ATT) {
  case ATT_ArrayRank:
''',
     '''  switch(ATT) {
  case ATT_XnuTypeSummary:
    return XnuTypeSummary(Self, T, KeyLoc);
  case ATT_XnuTypeSignature:
    return 0;
  case ATT_ArrayRank:
''')

edit(sema,
     '''  QualType T = TSInfo->getType();

  // FIXME: This should likely be tracked as an APInt to remove any host
''',
     '''  QualType T = TSInfo->getType();

  // __builtin_xnu_type_signature(T) is a string literal once T is known, and
  // until then a const char * (kalloc.h puts it in a ?: with "")
  if (ATT == ATT_XnuTypeSignature) {
    if (T->isDependentType())
      return new (Context) ArrayTypeTraitExpr(
          KWLoc, ATT, TSInfo, 0, DimExpr, RParen,
          Context.getPointerType(Context.CharTy.withConst()));
    std::string Sig = XnuTypeSignature(*this, T, KWLoc);
    QualType StrTy = Context.getStringLiteralArrayType(Context.CharTy,
                                                       Sig.size());
    return StringLiteral::Create(Context, Sig, StringLiteralKind::Ordinary,
                                 /*Pascal=*/false, StrTy, KWLoc);
  }

  // FIXME: This should likely be tracked as an APInt to remove any host
''')

edit(sema,
     '''  case BTT_IsSame:
    return Self.Context.hasSameType(LhsT, RhsT);
''',
     '''  case BTT_IsSame:
    return Self.Context.hasSameType(LhsT, RhsT);
  case BTT_XnuTypesCompatible:
    if (Self.Context.hasSameType(LhsT.getUnqualifiedType(),
                                 RhsT.getUnqualifiedType()))
      return true;
    if (LhsT->isVoidType() || RhsT->isVoidType())
      return false;
    return XnuTypeSignature(Self, LhsT, KeyLoc) ==
           XnuTypeSignature(Self, RhsT, KeyLoc);
''')

# Instantiating an array type trait without a dimension (__array_rank, the xnu traits) gave back the
# template's own expression once the dimension (none) was unchanged, even though the queried type had
# changed: IOKit_is_data_v<unsigned char> stayed value-dependent and codegen recursed forever on
# IOKit_typed_allocator<unsigned char, IOKit_is_data_v<unsigned char>> (IOUserServer.cpp). Past the type
# check above, the type has changed, so rebuild.
edit('lib/Sema/TreeTransform.h',
     '''    if (!getDerived().AlwaysRebuild() && SubExpr.get() == E->getDimensionExpression())
      return E;
  }

  return getDerived().RebuildArrayTypeTrait(''',
     '''  }

  // xnu: the queried type has changed; rebuild whatever the dimension
  return getDerived().RebuildArrayTypeTrait(''')

# The driver asks for -fregister-global-dtors-with-atexit on Darwin except for kernels and kexts, but cc1's
# default (CodeGenOptions.def) is on, so -fapple-kext code still registered its global destructors with
# __cxa_atexit, which the kernel does not have. Default off, as Options.td declares (DefaultFalse).
edit('include/clang/Basic/CodeGenOptions.def',
     'CODEGENOPT(RegisterGlobalDtorsWithAtExit, 1, 1)',
     'CODEGENOPT(RegisterGlobalDtorsWithAtExit, 1, 0)')
print('patched', src)
