//===--- CGExpr.cpp - Emit LLVM Code from Expressions ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Expr nodes as LLVM code.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CGCXXABI.h"
#include "CGCall.h"
#include "CGDebugInfo.h"
#include "CGObjCRuntime.h"
#include "CGOpenMPRuntime.h"
#include "CGRecordLayout.h"
#include "CodeGenModule.h"
#include "TargetInfo.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/SanitizerStats.h"
#include "llvm/Transforms/Utils/HexTypeUtil.h"
#include "llvm/Transforms/IPO/CastSanTools.h"

using namespace clang;
using namespace CodeGen;

//===--------------------------------------------------------------------===//
//                        Miscellaneous Helper Methods
//===--------------------------------------------------------------------===//

llvm::Value *CodeGenFunction::EmitCastToVoidPtr(llvm::Value *value) {
  unsigned addressSpace =
    cast<llvm::PointerType>(value->getType())->getAddressSpace();

  llvm::PointerType *destType = Int8PtrTy;
  if (addressSpace)
    destType = llvm::Type::getInt8PtrTy(getLLVMContext(), addressSpace);

  if (value->getType() == destType) return value;
  return Builder.CreateBitCast(value, destType);
}

/// CreateTempAlloca - This creates a alloca and inserts it into the entry
/// block.
Address CodeGenFunction::CreateTempAlloca(llvm::Type *Ty, CharUnits Align,
                                          const Twine &Name) {
  auto Alloca = CreateTempAlloca(Ty, Name);
  Alloca->setAlignment(Align.getQuantity());
  return Address(Alloca, Align);
}

/// CreateTempAlloca - This creates a alloca and inserts it into the entry
/// block.
llvm::AllocaInst *CodeGenFunction::CreateTempAlloca(llvm::Type *Ty,
                                                    const Twine &Name) {
  if (!Builder.isNamePreserving())
    return new llvm::AllocaInst(Ty, nullptr, "", AllocaInsertPt);
  return new llvm::AllocaInst(Ty, nullptr, Name, AllocaInsertPt);
}

/// CreateDefaultAlignTempAlloca - This creates an alloca with the
/// default alignment of the corresponding LLVM type, which is *not*
/// guaranteed to be related in any way to the expected alignment of
/// an AST type that might have been lowered to Ty.
Address CodeGenFunction::CreateDefaultAlignTempAlloca(llvm::Type *Ty,
                                                      const Twine &Name) {
  CharUnits Align =
    CharUnits::fromQuantity(CGM.getDataLayout().getABITypeAlignment(Ty));
  return CreateTempAlloca(Ty, Align, Name);
}

void CodeGenFunction::InitTempAlloca(Address Var, llvm::Value *Init) {
  assert(isa<llvm::AllocaInst>(Var.getPointer()));
  auto *Store = new llvm::StoreInst(Init, Var.getPointer());
  Store->setAlignment(Var.getAlignment().getQuantity());
  llvm::BasicBlock *Block = AllocaInsertPt->getParent();
  Block->getInstList().insertAfter(AllocaInsertPt->getIterator(), Store);
}

Address CodeGenFunction::CreateIRTemp(QualType Ty, const Twine &Name) {
  CharUnits Align = getContext().getTypeAlignInChars(Ty);
  return CreateTempAlloca(ConvertType(Ty), Align, Name);
}

Address CodeGenFunction::CreateMemTemp(QualType Ty, const Twine &Name) {
  // FIXME: Should we prefer the preferred type alignment here?
  return CreateMemTemp(Ty, getContext().getTypeAlignInChars(Ty), Name);
}

Address CodeGenFunction::CreateMemTemp(QualType Ty, CharUnits Align,
                                       const Twine &Name) {
  return CreateTempAlloca(ConvertTypeForMem(Ty), Align, Name);
}

/// EvaluateExprAsBool - Perform the usual unary conversions on the specified
/// expression and compare the result against zero, returning an Int1Ty value.
llvm::Value *CodeGenFunction::EvaluateExprAsBool(const Expr *E) {
  PGO.setCurrentStmt(E);
  if (const MemberPointerType *MPT = E->getType()->getAs<MemberPointerType>()) {
    llvm::Value *MemPtr = EmitScalarExpr(E);
    return CGM.getCXXABI().EmitMemberPointerIsNotNull(*this, MemPtr, MPT);
  }

  QualType BoolTy = getContext().BoolTy;
  SourceLocation Loc = E->getExprLoc();
  if (!E->getType()->isAnyComplexType())
    return EmitScalarConversion(EmitScalarExpr(E), E->getType(), BoolTy, Loc);

  return EmitComplexToScalarConversion(EmitComplexExpr(E), E->getType(), BoolTy,
                                       Loc);
}

/// EmitIgnoredExpr - Emit code to compute the specified expression,
/// ignoring the result.
void CodeGenFunction::EmitIgnoredExpr(const Expr *E) {
  if (E->isRValue())
    return (void) EmitAnyExpr(E, AggValueSlot::ignored(), true);

  // Just emit it as an l-value and drop the result.
  EmitLValue(E);
}

/// EmitAnyExpr - Emit code to compute the specified expression which
/// can have any type.  The result is returned as an RValue struct.
/// If this is an aggregate expression, AggSlot indicates where the
/// result should be returned.
RValue CodeGenFunction::EmitAnyExpr(const Expr *E,
                                    AggValueSlot aggSlot,
                                    bool ignoreResult) {
  switch (getEvaluationKind(E->getType())) {
  case TEK_Scalar:
    return RValue::get(EmitScalarExpr(E, ignoreResult));
  case TEK_Complex:
    return RValue::getComplex(EmitComplexExpr(E, ignoreResult, ignoreResult));
  case TEK_Aggregate:
    if (!ignoreResult && aggSlot.isIgnored())
      aggSlot = CreateAggTemp(E->getType(), "agg-temp");
    EmitAggExpr(E, aggSlot);
    return aggSlot.asRValue();
  }
  llvm_unreachable("bad evaluation kind");
}

/// EmitAnyExprToTemp - Similary to EmitAnyExpr(), however, the result will
/// always be accessible even if no aggregate location is provided.
RValue CodeGenFunction::EmitAnyExprToTemp(const Expr *E) {
  AggValueSlot AggSlot = AggValueSlot::ignored();

  if (hasAggregateEvaluationKind(E->getType()))
    AggSlot = CreateAggTemp(E->getType(), "agg.tmp");
  return EmitAnyExpr(E, AggSlot);
}

/// EmitAnyExprToMem - Evaluate an expression into a given memory
/// location.
void CodeGenFunction::EmitAnyExprToMem(const Expr *E,
                                       Address Location,
                                       Qualifiers Quals,
                                       bool IsInit) {
  // FIXME: This function should take an LValue as an argument.
  switch (getEvaluationKind(E->getType())) {
  case TEK_Complex:
    EmitComplexExprIntoLValue(E, MakeAddrLValue(Location, E->getType()),
                              /*isInit*/ false);
    return;

  case TEK_Aggregate: {
    EmitAggExpr(E, AggValueSlot::forAddr(Location, Quals,
                                         AggValueSlot::IsDestructed_t(IsInit),
                                         AggValueSlot::DoesNotNeedGCBarriers,
                                         AggValueSlot::IsAliased_t(!IsInit)));
    return;
  }

  case TEK_Scalar: {
    RValue RV = RValue::get(EmitScalarExpr(E, /*Ignore*/ false));
    LValue LV = MakeAddrLValue(Location, E->getType());
    EmitStoreThroughLValue(RV, LV);
    return;
  }
  }
  llvm_unreachable("bad evaluation kind");
}

static void
pushTemporaryCleanup(CodeGenFunction &CGF, const MaterializeTemporaryExpr *M,
                     const Expr *E, Address ReferenceTemporary) {
  // Objective-C++ ARC:
  //   If we are binding a reference to a temporary that has ownership, we
  //   need to perform retain/release operations on the temporary.
  //
  // FIXME: This should be looking at E, not M.
  if (auto Lifetime = M->getType().getObjCLifetime()) {
    switch (Lifetime) {
    case Qualifiers::OCL_None:
    case Qualifiers::OCL_ExplicitNone:
      // Carry on to normal cleanup handling.
      break;

    case Qualifiers::OCL_Autoreleasing:
      // Nothing to do; cleaned up by an autorelease pool.
      return;

    case Qualifiers::OCL_Strong:
    case Qualifiers::OCL_Weak:
      switch (StorageDuration Duration = M->getStorageDuration()) {
      case SD_Static:
        // Note: we intentionally do not register a cleanup to release
        // the object on program termination.
        return;

      case SD_Thread:
        // FIXME: We should probably register a cleanup in this case.
        return;

      case SD_Automatic:
      case SD_FullExpression:
        CodeGenFunction::Destroyer *Destroy;
        CleanupKind CleanupKind;
        if (Lifetime == Qualifiers::OCL_Strong) {
          const ValueDecl *VD = M->getExtendingDecl();
          bool Precise =
              VD && isa<VarDecl>(VD) && VD->hasAttr<ObjCPreciseLifetimeAttr>();
          CleanupKind = CGF.getARCCleanupKind();
          Destroy = Precise ? &CodeGenFunction::destroyARCStrongPrecise
                            : &CodeGenFunction::destroyARCStrongImprecise;
        } else {
          // __weak objects always get EH cleanups; otherwise, exceptions
          // could cause really nasty crashes instead of mere leaks.
          CleanupKind = NormalAndEHCleanup;
          Destroy = &CodeGenFunction::destroyARCWeak;
        }
        if (Duration == SD_FullExpression)
          CGF.pushDestroy(CleanupKind, ReferenceTemporary,
                          M->getType(), *Destroy,
                          CleanupKind & EHCleanup);
        else
          CGF.pushLifetimeExtendedDestroy(CleanupKind, ReferenceTemporary,
                                          M->getType(),
                                          *Destroy, CleanupKind & EHCleanup);
        return;

      case SD_Dynamic:
        llvm_unreachable("temporary cannot have dynamic storage duration");
      }
      llvm_unreachable("unknown storage duration");
    }
  }

  CXXDestructorDecl *ReferenceTemporaryDtor = nullptr;
  if (const RecordType *RT =
          E->getType()->getBaseElementTypeUnsafe()->getAs<RecordType>()) {
    // Get the destructor for the reference temporary.
    auto *ClassDecl = cast<CXXRecordDecl>(RT->getDecl());
    if (!ClassDecl->hasTrivialDestructor())
      ReferenceTemporaryDtor = ClassDecl->getDestructor();
  }

  if (!ReferenceTemporaryDtor)
    return;

  // Call the destructor for the temporary.
  switch (M->getStorageDuration()) {
  case SD_Static:
  case SD_Thread: {
    llvm::Constant *CleanupFn;
    llvm::Constant *CleanupArg;
    if (E->getType()->isArrayType()) {
      CleanupFn = CodeGenFunction(CGF.CGM).generateDestroyHelper(
          ReferenceTemporary, E->getType(),
          CodeGenFunction::destroyCXXObject, CGF.getLangOpts().Exceptions,
          dyn_cast_or_null<VarDecl>(M->getExtendingDecl()));
      CleanupArg = llvm::Constant::getNullValue(CGF.Int8PtrTy);
    } else {
      CleanupFn = CGF.CGM.getAddrOfCXXStructor(ReferenceTemporaryDtor,
                                               StructorType::Complete);
      CleanupArg = cast<llvm::Constant>(ReferenceTemporary.getPointer());
    }
    CGF.CGM.getCXXABI().registerGlobalDtor(
        CGF, *cast<VarDecl>(M->getExtendingDecl()), CleanupFn, CleanupArg);
    break;
  }

  case SD_FullExpression:
    CGF.pushDestroy(NormalAndEHCleanup, ReferenceTemporary, E->getType(),
                    CodeGenFunction::destroyCXXObject,
                    CGF.getLangOpts().Exceptions);
    break;

  case SD_Automatic:
    CGF.pushLifetimeExtendedDestroy(NormalAndEHCleanup,
                                    ReferenceTemporary, E->getType(),
                                    CodeGenFunction::destroyCXXObject,
                                    CGF.getLangOpts().Exceptions);
    break;

  case SD_Dynamic:
    llvm_unreachable("temporary cannot have dynamic storage duration");
  }
}

static Address
createReferenceTemporary(CodeGenFunction &CGF,
                         const MaterializeTemporaryExpr *M, const Expr *Inner) {
  switch (M->getStorageDuration()) {
  case SD_FullExpression:
  case SD_Automatic: {
    // If we have a constant temporary array or record try to promote it into a
    // constant global under the same rules a normal constant would've been
    // promoted. This is easier on the optimizer and generally emits fewer
    // instructions.
    QualType Ty = Inner->getType();
    if (CGF.CGM.getCodeGenOpts().MergeAllConstants &&
        (Ty->isArrayType() || Ty->isRecordType()) &&
        CGF.CGM.isTypeConstant(Ty, true))
      if (llvm::Constant *Init = CGF.CGM.EmitConstantExpr(Inner, Ty, &CGF)) {
        auto *GV = new llvm::GlobalVariable(
            CGF.CGM.getModule(), Init->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, Init, ".ref.tmp");
        CharUnits alignment = CGF.getContext().getTypeAlignInChars(Ty);
        GV->setAlignment(alignment.getQuantity());
        // FIXME: Should we put the new global into a COMDAT?
        return Address(GV, alignment);
      }
    return CGF.CreateMemTemp(Ty, "ref.tmp");
  }
  case SD_Thread:
  case SD_Static:
    return CGF.CGM.GetAddrOfGlobalTemporary(M, Inner);

  case SD_Dynamic:
    llvm_unreachable("temporary can't have dynamic storage duration");
  }
  llvm_unreachable("unknown storage duration");
}

LValue CodeGenFunction::
EmitMaterializeTemporaryExpr(const MaterializeTemporaryExpr *M) {
  const Expr *E = M->GetTemporaryExpr();

    // FIXME: ideally this would use EmitAnyExprToMem, however, we cannot do so
    // as that will cause the lifetime adjustment to be lost for ARC
  auto ownership = M->getType().getObjCLifetime();
  if (ownership != Qualifiers::OCL_None &&
      ownership != Qualifiers::OCL_ExplicitNone) {
    Address Object = createReferenceTemporary(*this, M, E);
    if (auto *Var = dyn_cast<llvm::GlobalVariable>(Object.getPointer())) {
      Object = Address(llvm::ConstantExpr::getBitCast(Var,
                           ConvertTypeForMem(E->getType())
                             ->getPointerTo(Object.getAddressSpace())),
                       Object.getAlignment());
      // We should not have emitted the initializer for this temporary as a
      // constant.
      assert(!Var->hasInitializer());
      Var->setInitializer(CGM.EmitNullConstant(E->getType()));
    }
    LValue RefTempDst = MakeAddrLValue(Object, M->getType(),
                                       AlignmentSource::Decl);

    switch (getEvaluationKind(E->getType())) {
    default: llvm_unreachable("expected scalar or aggregate expression");
    case TEK_Scalar:
      EmitScalarInit(E, M->getExtendingDecl(), RefTempDst, false);
      break;
    case TEK_Aggregate: {
      EmitAggExpr(E, AggValueSlot::forAddr(Object,
                                           E->getType().getQualifiers(),
                                           AggValueSlot::IsDestructed,
                                           AggValueSlot::DoesNotNeedGCBarriers,
                                           AggValueSlot::IsNotAliased));
      break;
    }
    }

    pushTemporaryCleanup(*this, M, E, Object);
    return RefTempDst;
  }

  SmallVector<const Expr *, 2> CommaLHSs;
  SmallVector<SubobjectAdjustment, 2> Adjustments;
  E = E->skipRValueSubobjectAdjustments(CommaLHSs, Adjustments);

  for (const auto &Ignored : CommaLHSs)
    EmitIgnoredExpr(Ignored);

  if (const auto *opaque = dyn_cast<OpaqueValueExpr>(E)) {
    if (opaque->getType()->isRecordType()) {
      assert(Adjustments.empty());
      return EmitOpaqueValueLValue(opaque);
    }
  }

  // Create and initialize the reference temporary.
  Address Object = createReferenceTemporary(*this, M, E);
  if (auto *Var = dyn_cast<llvm::GlobalVariable>(Object.getPointer())) {
    Object = Address(llvm::ConstantExpr::getBitCast(
        Var, ConvertTypeForMem(E->getType())->getPointerTo()),
                     Object.getAlignment());
    // If the temporary is a global and has a constant initializer or is a
    // constant temporary that we promoted to a global, we may have already
    // initialized it.
    if (!Var->hasInitializer()) {
      Var->setInitializer(CGM.EmitNullConstant(E->getType()));
      EmitAnyExprToMem(E, Object, Qualifiers(), /*IsInit*/true);
    }
  } else {
    EmitAnyExprToMem(E, Object, Qualifiers(), /*IsInit*/true);
  }
  pushTemporaryCleanup(*this, M, E, Object);

  // Perform derived-to-base casts and/or field accesses, to get from the
  // temporary object we created (and, potentially, for which we extended
  // the lifetime) to the subobject we're binding the reference to.
  for (unsigned I = Adjustments.size(); I != 0; --I) {
    SubobjectAdjustment &Adjustment = Adjustments[I-1];
    switch (Adjustment.Kind) {
    case SubobjectAdjustment::DerivedToBaseAdjustment:
      Object =
          GetAddressOfBaseClass(Object, Adjustment.DerivedToBase.DerivedClass,
                                Adjustment.DerivedToBase.BasePath->path_begin(),
                                Adjustment.DerivedToBase.BasePath->path_end(),
                                /*NullCheckValue=*/ false, E->getExprLoc());
      break;

    case SubobjectAdjustment::FieldAdjustment: {
      LValue LV = MakeAddrLValue(Object, E->getType(),
                                 AlignmentSource::Decl);
      LV = EmitLValueForField(LV, Adjustment.Field);
      assert(LV.isSimple() &&
             "materialized temporary field is not a simple lvalue");
      Object = LV.getAddress();
      break;
    }

    case SubobjectAdjustment::MemberPointerAdjustment: {
      llvm::Value *Ptr = EmitScalarExpr(Adjustment.Ptr.RHS);
      Object = EmitCXXMemberDataPointerAddress(E, Object, Ptr,
                                               Adjustment.Ptr.MPT);
      break;
    }
    }
  }

  return MakeAddrLValue(Object, M->getType(), AlignmentSource::Decl);
}

RValue
CodeGenFunction::EmitReferenceBindingToExpr(const Expr *E) {
  // Emit the expression as an lvalue.
  LValue LV = EmitLValue(E);
  assert(LV.isSimple());
  llvm::Value *Value = LV.getPointer();

  if (sanitizePerformTypeCheck() && !E->getType()->isFunctionType()) {
    // C++11 [dcl.ref]p5 (as amended by core issue 453):
    //   If a glvalue to which a reference is directly bound designates neither
    //   an existing object or function of an appropriate type nor a region of
    //   storage of suitable size and alignment to contain an object of the
    //   reference's type, the behavior is undefined.
    QualType Ty = E->getType();
    EmitTypeCheck(TCK_ReferenceBinding, E->getExprLoc(), Value, Ty);
  }

  return RValue::get(Value);
}


/// getAccessedFieldNo - Given an encoded value and a result number, return the
/// input field number being accessed.
unsigned CodeGenFunction::getAccessedFieldNo(unsigned Idx,
                                             const llvm::Constant *Elts) {
  return cast<llvm::ConstantInt>(Elts->getAggregateElement(Idx))
      ->getZExtValue();
}

/// Emit the hash_16_bytes function from include/llvm/ADT/Hashing.h.
static llvm::Value *emitHash16Bytes(CGBuilderTy &Builder, llvm::Value *Low,
                                    llvm::Value *High) {
  llvm::Value *KMul = Builder.getInt64(0x9ddfea08eb382d69ULL);
  llvm::Value *K47 = Builder.getInt64(47);
  llvm::Value *A0 = Builder.CreateMul(Builder.CreateXor(Low, High), KMul);
  llvm::Value *A1 = Builder.CreateXor(Builder.CreateLShr(A0, K47), A0);
  llvm::Value *B0 = Builder.CreateMul(Builder.CreateXor(High, A1), KMul);
  llvm::Value *B1 = Builder.CreateXor(Builder.CreateLShr(B0, K47), B0);
  return Builder.CreateMul(B1, KMul);
}

bool CodeGenFunction::sanitizePerformTypeCheck() const {
  return SanOpts.has(SanitizerKind::Null) |
         SanOpts.has(SanitizerKind::Alignment) |
         SanOpts.has(SanitizerKind::ObjectSize) |
         SanOpts.has(SanitizerKind::Vptr);
}

void CodeGenFunction::EmitVTableCastCheck(Address V, const CXXRecordDecl *DerivedClassDecl,
                          const CXXRecordDecl *BaseClassDecl) {


  // get the highest in class-hierarchy primary virtual base of the "Base" in the BaseToDerived conversion.
  // The range in the vtable of this virtual base will be used for the runtime range check
  const CXXRecordDecl * VBaseDecl = BaseClassDecl;
  std::string BaseMangledName = CGM.getCXXABI().GetClassMangledName(BaseClassDecl);
  ASTContext & Context = BaseClassDecl->getASTContext();
  const ASTRecordLayout * Layout = &Context.getASTRecordLayout(VBaseDecl);

  while(!Layout->hasOwnVFPtr()) {
    VBaseDecl = Layout->getPrimaryBase();
	if (!VBaseDecl ) {
		std::cerr << "CastCheck: " << BaseMangledName << " does not have it's own Vtable, but also no Parent with one." << std::endl;
		return;
	}
    Layout = &Context.getASTRecordLayout(VBaseDecl);
  }

  // Determine the mangled name of the derived class (target of Cast Operation)
  // and also of the primary virtual class (highest in class-hierarchy with VTable)
  // These names will be used to uniquely identify the correct VTable ranges in
  // the llvm pass (P4 of SafeDispatch)
  std::string DerivedMangledName = CGM.getCXXABI().GetClassMangledName(DerivedClassDecl);
  std::string VBaseMangledName = CGM.getCXXABI().GetClassMangledName(VBaseDecl);

  if (!sd_isVtableName(DerivedMangledName) || ! sd_isVtableName(VBaseMangledName))
  {
    printf("Cast Check: Cast from %s to %s: one is not eligible for checking.");
    return;
  }

  printf("Cast Check: inserting cast check from %s to %s", VBaseMangledName.c_str(), DerivedMangledName.c_str());

  llvm::Value * isNull = Builder.CreateIsNull(V.getPointer());

  // create a Load-Operation for the first VTable pointer of the to-be-casted object.
  // this Value holds a reference to the VPointer at runtime and will be used in the range checks
  llvm::BasicBlock *TrapBlock = createBasicBlock("cast.check.trap");
  llvm::BasicBlock *CheckBlock = createBasicBlock("cast.check");
  llvm::BasicBlock *ContBlock = createBasicBlock("cast.check.cont");

  Builder.CreateCondBr(isNull, ContBlock, CheckBlock);

  EmitBlock(CheckBlock);

  llvm::Value * VPointer = GetVTablePtr(V, Int8PtrTy, DerivedClassDecl);

  // insert the Intrinsic holding all Metadata needed to create a range check
  llvm::Value * CastCheck = InsertCastInfo(VBaseMangledName, DerivedMangledName, VPointer);

  // create basic

  Builder.CreateCondBr(CastCheck, ContBlock, TrapBlock);

  EmitBlock(TrapBlock);
  Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::trap));
  Builder.CreateUnreachable();
  //Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::donothing));

  EmitBlock(ContBlock);
}

void CodeGenFunction::EmitTypeCheck(TypeCheckKind TCK, SourceLocation Loc,
                                    llvm::Value *Ptr, QualType Ty,
                                    CharUnits Alignment, bool SkipNullCheck) {
  if (!sanitizePerformTypeCheck())
    return;

  // Don't check pointers outside the default address space. The null check
  // isn't correct, the object-size check isn't supported by LLVM, and we can't
  // communicate the addresses to the runtime handler for the vptr check.
  if (Ptr->getType()->getPointerAddressSpace())
    return;

  SanitizerScope SanScope(this);

  SmallVector<std::pair<llvm::Value *, SanitizerMask>, 3> Checks;
  llvm::BasicBlock *Done = nullptr;

  bool AllowNullPointers = TCK == TCK_DowncastPointer || TCK == TCK_Upcast ||
                           TCK == TCK_UpcastToVirtualBase;
  if ((SanOpts.has(SanitizerKind::Null) || AllowNullPointers) &&
      !SkipNullCheck) {
    // The glvalue must not be an empty glvalue.
    llvm::Value *IsNonNull = Builder.CreateIsNotNull(Ptr);

    if (AllowNullPointers) {
      // When performing pointer casts, it's OK if the value is null.
      // Skip the remaining checks in that case.
      Done = createBasicBlock("null");
      llvm::BasicBlock *Rest = createBasicBlock("not.null");
      Builder.CreateCondBr(IsNonNull, Rest, Done);
      EmitBlock(Rest);
    } else {
      Checks.push_back(std::make_pair(IsNonNull, SanitizerKind::Null));
    }
  }

  if (SanOpts.has(SanitizerKind::ObjectSize) && !Ty->isIncompleteType()) {
    uint64_t Size = getContext().getTypeSizeInChars(Ty).getQuantity();

    // The glvalue must refer to a large enough storage region.
    // FIXME: If Address Sanitizer is enabled, insert dynamic instrumentation
    //        to check this.
    // FIXME: Get object address space
    llvm::Type *Tys[2] = { IntPtrTy, Int8PtrTy };
    llvm::Value *F = CGM.getIntrinsic(llvm::Intrinsic::objectsize, Tys);
    llvm::Value *Min = Builder.getFalse();
    llvm::Value *CastAddr = Builder.CreateBitCast(Ptr, Int8PtrTy);
    llvm::Value *LargeEnough =
        Builder.CreateICmpUGE(Builder.CreateCall(F, {CastAddr, Min}),
                              llvm::ConstantInt::get(IntPtrTy, Size));
    Checks.push_back(std::make_pair(LargeEnough, SanitizerKind::ObjectSize));
  }

  uint64_t AlignVal = 0;

  if (SanOpts.has(SanitizerKind::Alignment)) {
    AlignVal = Alignment.getQuantity();
    if (!Ty->isIncompleteType() && !AlignVal)
      AlignVal = getContext().getTypeAlignInChars(Ty).getQuantity();

    // The glvalue must be suitably aligned.
    if (AlignVal) {
      llvm::Value *Align =
          Builder.CreateAnd(Builder.CreatePtrToInt(Ptr, IntPtrTy),
                            llvm::ConstantInt::get(IntPtrTy, AlignVal - 1));
      llvm::Value *Aligned =
        Builder.CreateICmpEQ(Align, llvm::ConstantInt::get(IntPtrTy, 0));
      Checks.push_back(std::make_pair(Aligned, SanitizerKind::Alignment));
    }
  }

  if (Checks.size() > 0) {
    llvm::Constant *StaticData[] = {
     EmitCheckSourceLocation(Loc),
      EmitCheckTypeDescriptor(Ty),
      llvm::ConstantInt::get(SizeTy, AlignVal),
      llvm::ConstantInt::get(Int8Ty, TCK)
    };
    EmitCheck(Checks, "type_mismatch", StaticData, Ptr);
  }

  // If possible, check that the vptr indicates that there is a subobject of
  // type Ty at offset zero within this object.
  //
  // C++11 [basic.life]p5,6:
  //   [For storage which does not refer to an object within its lifetime]
  //   The program has undefined behavior if:
  //    -- the [pointer or glvalue] is used to access a non-static data member
  //       or call a non-static member function
  CXXRecordDecl *RD = Ty->getAsCXXRecordDecl();
  if (SanOpts.has(SanitizerKind::Vptr) &&
      (TCK == TCK_MemberAccess || TCK == TCK_MemberCall ||
       TCK == TCK_DowncastPointer || TCK == TCK_DowncastReference ||
       TCK == TCK_UpcastToVirtualBase) &&
      RD && RD->hasDefinition() && RD->isDynamicClass()) {
    // Compute a hash of the mangled name of the type.
    //
    // FIXME: This is not guaranteed to be deterministic! Move to a
    //        fingerprinting mechanism once LLVM provides one. For the time
    //        being the implementation happens to be deterministic.
    SmallString<64> MangledName;
    llvm::raw_svector_ostream Out(MangledName);
    CGM.getCXXABI().getMangleContext().mangleCXXRTTI(Ty.getUnqualifiedType(),
                                                     Out);

    // Blacklist based on the mangled type.
    if (!CGM.getContext().getSanitizerBlacklist().isBlacklistedType(
            Out.str())) {
      llvm::hash_code TypeHash = hash_value(Out.str());

      // Load the vptr, and compute hash_16_bytes(TypeHash, vptr).
      llvm::Value *Low = llvm::ConstantInt::get(Int64Ty, TypeHash);
      llvm::Type *VPtrTy = llvm::PointerType::get(IntPtrTy, 0);
      Address VPtrAddr(Builder.CreateBitCast(Ptr, VPtrTy), getPointerAlign());
      llvm::Value *VPtrVal = Builder.CreateLoad(VPtrAddr);
      llvm::Value *High = Builder.CreateZExt(VPtrVal, Int64Ty);

      llvm::Value *Hash = emitHash16Bytes(Builder, Low, High);
      Hash = Builder.CreateTrunc(Hash, IntPtrTy);

      // Look the hash up in our cache.
      const int CacheSize = 128;
      llvm::Type *HashTable = llvm::ArrayType::get(IntPtrTy, CacheSize);
      llvm::Value *Cache = CGM.CreateRuntimeVariable(HashTable,
                                                     "__ubsan_vptr_type_cache");
      llvm::Value *Slot = Builder.CreateAnd(Hash,
                                            llvm::ConstantInt::get(IntPtrTy,
                                                                   CacheSize-1));
      llvm::Value *Indices[] = { Builder.getInt32(0), Slot };
      llvm::Value *CacheVal =
        Builder.CreateAlignedLoad(Builder.CreateInBoundsGEP(Cache, Indices),
                                  getPointerAlign());

      // If the hash isn't in the cache, call a runtime handler to perform the
      // hard work of checking whether the vptr is for an object of the right
      // type. This will either fill in the cache and return, or produce a
      // diagnostic.
      llvm::Value *EqualHash = Builder.CreateICmpEQ(CacheVal, Hash);
      llvm::Constant *StaticData[] = {
        EmitCheckSourceLocation(Loc),
        EmitCheckTypeDescriptor(Ty),
        CGM.GetAddrOfRTTIDescriptor(Ty.getUnqualifiedType()),
        llvm::ConstantInt::get(Int8Ty, TCK)
      };
      llvm::Value *DynamicData[] = { Ptr, Hash };
      EmitCheck(std::make_pair(EqualHash, SanitizerKind::Vptr),
                "dynamic_type_cache_miss", StaticData, DynamicData);
    }
  }

  if (Done) {
    Builder.CreateBr(Done);
    EmitBlock(Done);
  }
}

/// Determine whether this expression refers to a flexible array member in a
/// struct. We disable array bounds checks for such members.
static bool isFlexibleArrayMemberExpr(const Expr *E) {
  // For compatibility with existing code, we treat arrays of length 0 or
  // 1 as flexible array members.
  const ArrayType *AT = E->getType()->castAsArrayTypeUnsafe();
  if (const auto *CAT = dyn_cast<ConstantArrayType>(AT)) {
    if (CAT->getSize().ugt(1))
      return false;
  } else if (!isa<IncompleteArrayType>(AT))
    return false;

  E = E->IgnoreParens();

  // A flexible array member must be the last member in the class.
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    // FIXME: If the base type of the member expr is not FD->getParent(),
    // this should not be treated as a flexible array member access.
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      RecordDecl::field_iterator FI(
          DeclContext::decl_iterator(const_cast<FieldDecl *>(FD)));
      return ++FI == FD->getParent()->field_end();
    }
  }

  return false;
}

/// If Base is known to point to the start of an array, return the length of
/// that array. Return 0 if the length cannot be determined.
static llvm::Value *getArrayIndexingBound(
    CodeGenFunction &CGF, const Expr *Base, QualType &IndexedType) {
  // For the vector indexing extension, the bound is the number of elements.
  if (const VectorType *VT = Base->getType()->getAs<VectorType>()) {
    IndexedType = Base->getType();
    return CGF.Builder.getInt32(VT->getNumElements());
  }

  Base = Base->IgnoreParens();

  if (const auto *CE = dyn_cast<CastExpr>(Base)) {
    if (CE->getCastKind() == CK_ArrayToPointerDecay &&
        !isFlexibleArrayMemberExpr(CE->getSubExpr())) {
      IndexedType = CE->getSubExpr()->getType();
      const ArrayType *AT = IndexedType->castAsArrayTypeUnsafe();
      if (const auto *CAT = dyn_cast<ConstantArrayType>(AT))
        return CGF.Builder.getInt(CAT->getSize());
      else if (const auto *VAT = dyn_cast<VariableArrayType>(AT))
        return CGF.getVLASize(VAT).first;
    }
  }

  return nullptr;
}

void CodeGenFunction::EmitBoundsCheck(const Expr *E, const Expr *Base,
                                      llvm::Value *Index, QualType IndexType,
                                      bool Accessed) {
  assert(SanOpts.has(SanitizerKind::ArrayBounds) &&
         "should not be called unless adding bounds checks");
  SanitizerScope SanScope(this);

  QualType IndexedType;
  llvm::Value *Bound = getArrayIndexingBound(*this, Base, IndexedType);
  if (!Bound)
    return;

  bool IndexSigned = IndexType->isSignedIntegerOrEnumerationType();
  llvm::Value *IndexVal = Builder.CreateIntCast(Index, SizeTy, IndexSigned);
  llvm::Value *BoundVal = Builder.CreateIntCast(Bound, SizeTy, false);

  llvm::Constant *StaticData[] = {
    EmitCheckSourceLocation(E->getExprLoc()),
    EmitCheckTypeDescriptor(IndexedType),
    EmitCheckTypeDescriptor(IndexType)
  };
  llvm::Value *Check = Accessed ? Builder.CreateICmpULT(IndexVal, BoundVal)
                                : Builder.CreateICmpULE(IndexVal, BoundVal);
  EmitCheck(std::make_pair(Check, SanitizerKind::ArrayBounds), "out_of_bounds",
            StaticData, Index);
}


CodeGenFunction::ComplexPairTy CodeGenFunction::
EmitComplexPrePostIncDec(const UnaryOperator *E, LValue LV,
                         bool isInc, bool isPre) {
  ComplexPairTy InVal = EmitLoadOfComplex(LV, E->getExprLoc());

  llvm::Value *NextVal;
  if (isa<llvm::IntegerType>(InVal.first->getType())) {
    uint64_t AmountVal = isInc ? 1 : -1;
    NextVal = llvm::ConstantInt::get(InVal.first->getType(), AmountVal, true);

    // Add the inc/dec to the real part.
    NextVal = Builder.CreateAdd(InVal.first, NextVal, isInc ? "inc" : "dec");
  } else {
    QualType ElemTy = E->getType()->getAs<ComplexType>()->getElementType();
    llvm::APFloat FVal(getContext().getFloatTypeSemantics(ElemTy), 1);
    if (!isInc)
      FVal.changeSign();
    NextVal = llvm::ConstantFP::get(getLLVMContext(), FVal);

    // Add the inc/dec to the real part.
    NextVal = Builder.CreateFAdd(InVal.first, NextVal, isInc ? "inc" : "dec");
  }

  ComplexPairTy IncVal(NextVal, InVal.second);

  // Store the updated result through the lvalue.
  EmitStoreOfComplex(IncVal, LV, /*init*/ false);

  // If this is a postinc, return the value read from memory, otherwise use the
  // updated value.
  return isPre ? IncVal : InVal;
}

void CodeGenModule::EmitExplicitCastExprType(const ExplicitCastExpr *E,
                                             CodeGenFunction *CGF) {
  // Bind VLAs in the cast type.
  if (CGF && E->getType()->isVariablyModifiedType())
    CGF->EmitVariablyModifiedType(E->getType());

  if (CGDebugInfo *DI = getModuleDebugInfo())
    DI->EmitExplicitCastType(E->getType());
}

//===----------------------------------------------------------------------===//
//                         LValue Expression Emission
//===----------------------------------------------------------------------===//

/// EmitPointerWithAlignment - Given an expression of pointer type, try to
/// derive a more accurate bound on the alignment of the pointer.
Address CodeGenFunction::EmitPointerWithAlignment(const Expr *E,
                                                  AlignmentSource  *Source) {
  // We allow this with ObjC object pointers because of fragile ABIs.
  assert(E->getType()->isPointerType() ||
         E->getType()->isObjCObjectPointerType());
  E = E->IgnoreParens();

  // Casts:
  if (const CastExpr *CE = dyn_cast<CastExpr>(E)) {
    if (const auto *ECE = dyn_cast<ExplicitCastExpr>(CE))
      CGM.EmitExplicitCastExprType(ECE, this);

    switch (CE->getCastKind()) {
    // Non-converting casts (but not C's implicit conversion from void*).
    case CK_BitCast:
    case CK_NoOp:
      if (auto PtrTy = CE->getSubExpr()->getType()->getAs<PointerType>()) {
        if (PtrTy->getPointeeType()->isVoidType())
          break;

        AlignmentSource InnerSource;
        Address Addr = EmitPointerWithAlignment(CE->getSubExpr(), &InnerSource);
        if (Source) *Source = InnerSource;

        // If this is an explicit bitcast, and the source l-value is
        // opaque, honor the alignment of the casted-to type.
        if (isa<ExplicitCastExpr>(CE) &&
            InnerSource != AlignmentSource::Decl) {
          Addr = Address(Addr.getPointer(),
                         getNaturalPointeeTypeAlignment(E->getType(), Source));
        }

        if (SanOpts.has(SanitizerKind::CFIUnrelatedCast) &&
            CE->getCastKind() == CK_BitCast) {
          if (auto PT = E->getType()->getAs<PointerType>())
            EmitVTablePtrCheckForCast(PT->getPointeeType(), Addr.getPointer(),
                                      /*MayBeNull=*/true,
                                      CodeGenFunction::CFITCK_UnrelatedCast,
                                      CE->getLocStart());
        }

        return Builder.CreateBitCast(Addr, ConvertType(E->getType()));
      }
      break;

    // Array-to-pointer decay.
    case CK_ArrayToPointerDecay:
      return EmitArrayToPointerDecay(CE->getSubExpr(), Source);

    // Derived-to-base conversions.
    case CK_UncheckedDerivedToBase:
    case CK_DerivedToBase: {
      Address Addr = EmitPointerWithAlignment(CE->getSubExpr(), Source);
      auto Derived = CE->getSubExpr()->getType()->getPointeeCXXRecordDecl();
      return GetAddressOfBaseClass(Addr, Derived,
                                   CE->path_begin(), CE->path_end(),
                                   ShouldNullCheckClassCastValue(CE),
                                   CE->getExprLoc());
    }

    // TODO: Is there any reason to treat base-to-derived conversions
    // specially?
    default:
      break;
    }
  }

  // Unary &.
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf) {
      LValue LV = EmitLValue(UO->getSubExpr());
      if (Source) *Source = LV.getAlignmentSource();
      return LV.getAddress();
    }
  }

  // TODO: conditional operators, comma.

  // Otherwise, use the alignment of the type.
  CharUnits Align = getNaturalPointeeTypeAlignment(E->getType(), Source);
  return Address(EmitScalarExpr(E), Align);
}

RValue CodeGenFunction::GetUndefRValue(QualType Ty) {
  if (Ty->isVoidType())
    return RValue::get(nullptr);

  switch (getEvaluationKind(Ty)) {
  case TEK_Complex: {
    llvm::Type *EltTy =
      ConvertType(Ty->castAs<ComplexType>()->getElementType());
    llvm::Value *U = llvm::UndefValue::get(EltTy);
    return RValue::getComplex(std::make_pair(U, U));
  }

  // If this is a use of an undefined aggregate type, the aggregate must have an
  // identifiable address.  Just because the contents of the value are undefined
  // doesn't mean that the address can't be taken and compared.
  case TEK_Aggregate: {
    Address DestPtr = CreateMemTemp(Ty, "undef.agg.tmp");
    return RValue::getAggregate(DestPtr);
  }

  case TEK_Scalar:
    return RValue::get(llvm::UndefValue::get(ConvertType(Ty)));
  }
  llvm_unreachable("bad evaluation kind");
}

RValue CodeGenFunction::EmitUnsupportedRValue(const Expr *E,
                                              const char *Name) {
  ErrorUnsupported(E, Name);
  return GetUndefRValue(E->getType());
}

LValue CodeGenFunction::EmitUnsupportedLValue(const Expr *E,
                                              const char *Name) {
  ErrorUnsupported(E, Name);
  llvm::Type *Ty = llvm::PointerType::getUnqual(ConvertType(E->getType()));
  return MakeAddrLValue(Address(llvm::UndefValue::get(Ty), CharUnits::One()),
                        E->getType());
}

LValue CodeGenFunction::EmitCheckedLValue(const Expr *E, TypeCheckKind TCK) {
  LValue LV;
  if (SanOpts.has(SanitizerKind::ArrayBounds) && isa<ArraySubscriptExpr>(E))
    LV = EmitArraySubscriptExpr(cast<ArraySubscriptExpr>(E), /*Accessed*/true);
  else
    LV = EmitLValue(E);
  if (!isa<DeclRefExpr>(E) && !LV.isBitField() && LV.isSimple())
    EmitTypeCheck(TCK, E->getExprLoc(), LV.getPointer(),
                  E->getType(), LV.getAlignment());
  return LV;
}

/// EmitLValue - Emit code to compute a designator that specifies the location
/// of the expression.
///
/// This can return one of two things: a simple address or a bitfield reference.
/// In either case, the LLVM Value* in the LValue structure is guaranteed to be
/// an LLVM pointer type.
///
/// If this returns a bitfield reference, nothing about the pointee type of the
/// LLVM value is known: For example, it may not be a pointer to an integer.
///
/// If this returns a normal address, and if the lvalue's C type is fixed size,
/// this method guarantees that the returned pointer type will point to an LLVM
/// type of the same size of the lvalue's type.  If the lvalue has a variable
/// length type, this is not possible.
///
LValue CodeGenFunction::EmitLValue(const Expr *E) {
  ApplyDebugLocation DL(*this, E);
  switch (E->getStmtClass()) {
  default: return EmitUnsupportedLValue(E, "l-value expression");

  case Expr::ObjCPropertyRefExprClass:
    llvm_unreachable("cannot emit a property reference directly");

  case Expr::ObjCSelectorExprClass:
    return EmitObjCSelectorLValue(cast<ObjCSelectorExpr>(E));
  case Expr::ObjCIsaExprClass:
    return EmitObjCIsaExpr(cast<ObjCIsaExpr>(E));
  case Expr::BinaryOperatorClass:
    return EmitBinaryOperatorLValue(cast<BinaryOperator>(E));
  case Expr::CompoundAssignOperatorClass: {
    QualType Ty = E->getType();
    if (const AtomicType *AT = Ty->getAs<AtomicType>())
      Ty = AT->getValueType();
    if (!Ty->isAnyComplexType())
      return EmitCompoundAssignmentLValue(cast<CompoundAssignOperator>(E));
    return EmitComplexCompoundAssignmentLValue(cast<CompoundAssignOperator>(E));
  }
  case Expr::CallExprClass:
  case Expr::CXXMemberCallExprClass:
  case Expr::CXXOperatorCallExprClass:
  case Expr::UserDefinedLiteralClass:
    return EmitCallExprLValue(cast<CallExpr>(E));
  case Expr::VAArgExprClass:
    return EmitVAArgExprLValue(cast<VAArgExpr>(E));
  case Expr::DeclRefExprClass:
    return EmitDeclRefLValue(cast<DeclRefExpr>(E));
  case Expr::ParenExprClass:
    return EmitLValue(cast<ParenExpr>(E)->getSubExpr());
  case Expr::GenericSelectionExprClass:
    return EmitLValue(cast<GenericSelectionExpr>(E)->getResultExpr());
  case Expr::PredefinedExprClass:
    return EmitPredefinedLValue(cast<PredefinedExpr>(E));
  case Expr::StringLiteralClass:
    return EmitStringLiteralLValue(cast<StringLiteral>(E));
  case Expr::ObjCEncodeExprClass:
    return EmitObjCEncodeExprLValue(cast<ObjCEncodeExpr>(E));
  case Expr::PseudoObjectExprClass:
    return EmitPseudoObjectLValue(cast<PseudoObjectExpr>(E));
  case Expr::InitListExprClass:
    return EmitInitListLValue(cast<InitListExpr>(E));
  case Expr::CXXTemporaryObjectExprClass:
  case Expr::CXXConstructExprClass:
    return EmitCXXConstructLValue(cast<CXXConstructExpr>(E));
  case Expr::CXXBindTemporaryExprClass:
    return EmitCXXBindTemporaryLValue(cast<CXXBindTemporaryExpr>(E));
  case Expr::CXXUuidofExprClass:
    return EmitCXXUuidofLValue(cast<CXXUuidofExpr>(E));
  case Expr::LambdaExprClass:
    return EmitLambdaLValue(cast<LambdaExpr>(E));

  case Expr::ExprWithCleanupsClass: {
    const auto *cleanups = cast<ExprWithCleanups>(E);
    enterFullExpression(cleanups);
    RunCleanupsScope Scope(*this);
    return EmitLValue(cleanups->getSubExpr());
  }

  case Expr::CXXDefaultArgExprClass:
    return EmitLValue(cast<CXXDefaultArgExpr>(E)->getExpr());
  case Expr::CXXDefaultInitExprClass: {
    CXXDefaultInitExprScope Scope(*this);
    return EmitLValue(cast<CXXDefaultInitExpr>(E)->getExpr());
  }
  case Expr::CXXTypeidExprClass:
    return EmitCXXTypeidLValue(cast<CXXTypeidExpr>(E));

  case Expr::ObjCMessageExprClass:
    return EmitObjCMessageExprLValue(cast<ObjCMessageExpr>(E));
  case Expr::ObjCIvarRefExprClass:
    return EmitObjCIvarRefLValue(cast<ObjCIvarRefExpr>(E));
  case Expr::StmtExprClass:
    return EmitStmtExprLValue(cast<StmtExpr>(E));
  case Expr::UnaryOperatorClass:
    return EmitUnaryOpLValue(cast<UnaryOperator>(E));
  case Expr::ArraySubscriptExprClass:
    return EmitArraySubscriptExpr(cast<ArraySubscriptExpr>(E));
  case Expr::OMPArraySectionExprClass:
    return EmitOMPArraySectionExpr(cast<OMPArraySectionExpr>(E));
  case Expr::ExtVectorElementExprClass:
    return EmitExtVectorElementExpr(cast<ExtVectorElementExpr>(E));
  case Expr::MemberExprClass:
    return EmitMemberExpr(cast<MemberExpr>(E));
  case Expr::CompoundLiteralExprClass:
    return EmitCompoundLiteralLValue(cast<CompoundLiteralExpr>(E));
  case Expr::ConditionalOperatorClass:
    return EmitConditionalOperatorLValue(cast<ConditionalOperator>(E));
  case Expr::BinaryConditionalOperatorClass:
    return EmitConditionalOperatorLValue(cast<BinaryConditionalOperator>(E));
  case Expr::ChooseExprClass:
    return EmitLValue(cast<ChooseExpr>(E)->getChosenSubExpr());
  case Expr::OpaqueValueExprClass:
    return EmitOpaqueValueLValue(cast<OpaqueValueExpr>(E));
  case Expr::SubstNonTypeTemplateParmExprClass:
    return EmitLValue(cast<SubstNonTypeTemplateParmExpr>(E)->getReplacement());
  case Expr::ImplicitCastExprClass:
  case Expr::CStyleCastExprClass:
  case Expr::CXXFunctionalCastExprClass:
  case Expr::CXXStaticCastExprClass:
  case Expr::CXXDynamicCastExprClass:
  case Expr::CXXReinterpretCastExprClass:
  case Expr::CXXConstCastExprClass:
  case Expr::ObjCBridgedCastExprClass:
    return EmitCastLValue(cast<CastExpr>(E));

  case Expr::MaterializeTemporaryExprClass:
    return EmitMaterializeTemporaryExpr(cast<MaterializeTemporaryExpr>(E));
  }
}

/// Given an object of the given canonical type, can we safely copy a
/// value out of it based on its initializer?
static bool isConstantEmittableObjectType(QualType type) {
  assert(type.isCanonical());
  assert(!type->isReferenceType());

  // Must be const-qualified but non-volatile.
  Qualifiers qs = type.getLocalQualifiers();
  if (!qs.hasConst() || qs.hasVolatile()) return false;

  // Otherwise, all object types satisfy this except C++ classes with
  // mutable subobjects or non-trivial copy/destroy behavior.
  if (const auto *RT = dyn_cast<RecordType>(type))
    if (const auto *RD = dyn_cast<CXXRecordDecl>(RT->getDecl()))
      if (RD->hasMutableFields() || !RD->isTrivial())
        return false;

  return true;
}

/// Can we constant-emit a load of a reference to a variable of the
/// given type?  This is different from predicates like
/// Decl::isUsableInConstantExpressions because we do want it to apply
/// in situations that don't necessarily satisfy the language's rules
/// for this (e.g. C++'s ODR-use rules).  For example, we want to able
/// to do this with const float variables even if those variables
/// aren't marked 'constexpr'.
enum ConstantEmissionKind {
  CEK_None,
  CEK_AsReferenceOnly,
  CEK_AsValueOrReference,
  CEK_AsValueOnly
};
static ConstantEmissionKind checkVarTypeForConstantEmission(QualType type) {
  type = type.getCanonicalType();
  if (const auto *ref = dyn_cast<ReferenceType>(type)) {
    if (isConstantEmittableObjectType(ref->getPointeeType()))
      return CEK_AsValueOrReference;
    return CEK_AsReferenceOnly;
  }
  if (isConstantEmittableObjectType(type))
    return CEK_AsValueOnly;
  return CEK_None;
}

/// Try to emit a reference to the given value without producing it as
/// an l-value.  This is actually more than an optimization: we can't
/// produce an l-value for variables that we never actually captured
/// in a block or lambda, which means const int variables or constexpr
/// literals or similar.
CodeGenFunction::ConstantEmission
CodeGenFunction::tryEmitAsConstant(DeclRefExpr *refExpr) {
  ValueDecl *value = refExpr->getDecl();

  // The value needs to be an enum constant or a constant variable.
  ConstantEmissionKind CEK;
  if (isa<ParmVarDecl>(value)) {
    CEK = CEK_None;
  } else if (auto *var = dyn_cast<VarDecl>(value)) {
    CEK = checkVarTypeForConstantEmission(var->getType());
  } else if (isa<EnumConstantDecl>(value)) {
    CEK = CEK_AsValueOnly;
  } else {
    CEK = CEK_None;
  }
  if (CEK == CEK_None) return ConstantEmission();

  Expr::EvalResult result;
  bool resultIsReference;
  QualType resultType;

  // It's best to evaluate all the way as an r-value if that's permitted.
  if (CEK != CEK_AsReferenceOnly &&
      refExpr->EvaluateAsRValue(result, getContext())) {
    resultIsReference = false;
    resultType = refExpr->getType();

  // Otherwise, try to evaluate as an l-value.
  } else if (CEK != CEK_AsValueOnly &&
             refExpr->EvaluateAsLValue(result, getContext())) {
    resultIsReference = true;
    resultType = value->getType();

  // Failure.
  } else {
    return ConstantEmission();
  }

  // In any case, if the initializer has side-effects, abandon ship.
  if (result.HasSideEffects)
    return ConstantEmission();

  // Emit as a constant.
  llvm::Constant *C = CGM.EmitConstantValue(result.Val, resultType, this);

  // Make sure we emit a debug reference to the global variable.
  // This should probably fire even for
  if (isa<VarDecl>(value)) {
    if (!getContext().DeclMustBeEmitted(cast<VarDecl>(value)))
      EmitDeclRefExprDbgValue(refExpr, C);
  } else {
    assert(isa<EnumConstantDecl>(value));
    EmitDeclRefExprDbgValue(refExpr, C);
  }

  // If we emitted a reference constant, we need to dereference that.
  if (resultIsReference)
    return ConstantEmission::forReference(C);

  return ConstantEmission::forValue(C);
}

llvm::Value *CodeGenFunction::EmitLoadOfScalar(LValue lvalue,
                                               SourceLocation Loc) {
  return EmitLoadOfScalar(lvalue.getAddress(), lvalue.isVolatile(),
                          lvalue.getType(), Loc, lvalue.getAlignmentSource(),
                          lvalue.getTBAAInfo(),
                          lvalue.getTBAABaseType(), lvalue.getTBAAOffset(),
                          lvalue.isNontemporal());
}

static bool hasBooleanRepresentation(QualType Ty) {
  if (Ty->isBooleanType())
    return true;

  if (const EnumType *ET = Ty->getAs<EnumType>())
    return ET->getDecl()->getIntegerType()->isBooleanType();

  if (const AtomicType *AT = Ty->getAs<AtomicType>())
    return hasBooleanRepresentation(AT->getValueType());

  return false;
}

static bool getRangeForType(CodeGenFunction &CGF, QualType Ty,
                            llvm::APInt &Min, llvm::APInt &End,
                            bool StrictEnums) {
  const EnumType *ET = Ty->getAs<EnumType>();
  bool IsRegularCPlusPlusEnum = CGF.getLangOpts().CPlusPlus && StrictEnums &&
                                ET && !ET->getDecl()->isFixed();
  bool IsBool = hasBooleanRepresentation(Ty);
  if (!IsBool && !IsRegularCPlusPlusEnum)
    return false;

  if (IsBool) {
    Min = llvm::APInt(CGF.getContext().getTypeSize(Ty), 0);
    End = llvm::APInt(CGF.getContext().getTypeSize(Ty), 2);
  } else {
    const EnumDecl *ED = ET->getDecl();
    llvm::Type *LTy = CGF.ConvertTypeForMem(ED->getIntegerType());
    unsigned Bitwidth = LTy->getScalarSizeInBits();
    unsigned NumNegativeBits = ED->getNumNegativeBits();
    unsigned NumPositiveBits = ED->getNumPositiveBits();

    if (NumNegativeBits) {
      unsigned NumBits = std::max(NumNegativeBits, NumPositiveBits + 1);
      assert(NumBits <= Bitwidth);
      End = llvm::APInt(Bitwidth, 1) << (NumBits - 1);
      Min = -End;
    } else {
      assert(NumPositiveBits <= Bitwidth);
      End = llvm::APInt(Bitwidth, 1) << NumPositiveBits;
      Min = llvm::APInt(Bitwidth, 0);
    }
  }
  return true;
}

llvm::MDNode *CodeGenFunction::getRangeForLoadFromType(QualType Ty) {
  llvm::APInt Min, End;
  if (!getRangeForType(*this, Ty, Min, End,
                       CGM.getCodeGenOpts().StrictEnums))
    return nullptr;

  llvm::MDBuilder MDHelper(getLLVMContext());
  return MDHelper.createRange(Min, End);
}

llvm::Value *CodeGenFunction::EmitLoadOfScalar(Address Addr, bool Volatile,
                                               QualType Ty,
                                               SourceLocation Loc,
                                               AlignmentSource AlignSource,
                                               llvm::MDNode *TBAAInfo,
                                               QualType TBAABaseType,
                                               uint64_t TBAAOffset,
                                               bool isNontemporal) {
  // For better performance, handle vector loads differently.
  if (Ty->isVectorType()) {
    const llvm::Type *EltTy = Addr.getElementType();

    const auto *VTy = cast<llvm::VectorType>(EltTy);

    // Handle vectors of size 3 like size 4 for better performance.
    if (VTy->getNumElements() == 3) {

      // Bitcast to vec4 type.
      llvm::VectorType *vec4Ty = llvm::VectorType::get(VTy->getElementType(),
                                                         4);
      Address Cast = Builder.CreateElementBitCast(Addr, vec4Ty, "castToVec4");
      // Now load value.
      llvm::Value *V = Builder.CreateLoad(Cast, Volatile, "loadVec4");

      // Shuffle vector to get vec3.
      V = Builder.CreateShuffleVector(V, llvm::UndefValue::get(vec4Ty),
                                      {0, 1, 2}, "extractVec");
      return EmitFromMemory(V, Ty);
    }
  }

  // Atomic operations have to be done on integral types.
  if (Ty->isAtomicType() || typeIsSuitableForInlineAtomic(Ty, Volatile)) {
    LValue lvalue =
      LValue::MakeAddr(Addr, Ty, getContext(), AlignSource, TBAAInfo);
    return EmitAtomicLoad(lvalue, Loc).getScalarVal();
  }

  llvm::LoadInst *Load = Builder.CreateLoad(Addr, Volatile);
  if (isNontemporal) {
    llvm::MDNode *Node = llvm::MDNode::get(
        Load->getContext(), llvm::ConstantAsMetadata::get(Builder.getInt32(1)));
    Load->setMetadata(CGM.getModule().getMDKindID("nontemporal"), Node);
  }
  if (TBAAInfo) {
    llvm::MDNode *TBAAPath = CGM.getTBAAStructTagInfo(TBAABaseType, TBAAInfo,
                                                      TBAAOffset);
    if (TBAAPath)
      CGM.DecorateInstructionWithTBAA(Load, TBAAPath,
                                      false /*ConvertTypeToTag*/);
  }

  bool NeedsBoolCheck =
      SanOpts.has(SanitizerKind::Bool) && hasBooleanRepresentation(Ty);
  bool NeedsEnumCheck =
      SanOpts.has(SanitizerKind::Enum) && Ty->getAs<EnumType>();
  if (NeedsBoolCheck || NeedsEnumCheck) {
    SanitizerScope SanScope(this);
    llvm::APInt Min, End;
    if (getRangeForType(*this, Ty, Min, End, true)) {
      --End;
      llvm::Value *Check;
      if (!Min)
        Check = Builder.CreateICmpULE(
          Load, llvm::ConstantInt::get(getLLVMContext(), End));
      else {
        llvm::Value *Upper = Builder.CreateICmpSLE(
          Load, llvm::ConstantInt::get(getLLVMContext(), End));
        llvm::Value *Lower = Builder.CreateICmpSGE(
          Load, llvm::ConstantInt::get(getLLVMContext(), Min));
        Check = Builder.CreateAnd(Upper, Lower);
      }
      llvm::Constant *StaticArgs[] = {
        EmitCheckSourceLocation(Loc),
        EmitCheckTypeDescriptor(Ty)
      };
      SanitizerMask Kind = NeedsEnumCheck ? SanitizerKind::Enum : SanitizerKind::Bool;
      EmitCheck(std::make_pair(Check, Kind), "load_invalid_value", StaticArgs,
                EmitCheckValue(Load));
    }
  } else if (CGM.getCodeGenOpts().OptimizationLevel > 0)
    if (llvm::MDNode *RangeInfo = getRangeForLoadFromType(Ty))
      Load->setMetadata(llvm::LLVMContext::MD_range, RangeInfo);

  return EmitFromMemory(Load, Ty);
}

llvm::Value *CodeGenFunction::EmitToMemory(llvm::Value *Value, QualType Ty) {
  // Bool has a different representation in memory than in registers.
  if (hasBooleanRepresentation(Ty)) {
    // This should really always be an i1, but sometimes it's already
    // an i8, and it's awkward to track those cases down.
    if (Value->getType()->isIntegerTy(1))
      return Builder.CreateZExt(Value, ConvertTypeForMem(Ty), "frombool");
    assert(Value->getType()->isIntegerTy(getContext().getTypeSize(Ty)) &&
           "wrong value rep of bool");
  }

  return Value;
}

llvm::Value *CodeGenFunction::EmitFromMemory(llvm::Value *Value, QualType Ty) {
  // Bool has a different representation in memory than in registers.
  if (hasBooleanRepresentation(Ty)) {
    assert(Value->getType()->isIntegerTy(getContext().getTypeSize(Ty)) &&
           "wrong value rep of bool");
    return Builder.CreateTrunc(Value, Builder.getInt1Ty(), "tobool");
  }

  return Value;
}

void CodeGenFunction::EmitStoreOfScalar(llvm::Value *Value, Address Addr,
                                        bool Volatile, QualType Ty,
                                        AlignmentSource AlignSource,
                                        llvm::MDNode *TBAAInfo,
                                        bool isInit, QualType TBAABaseType,
                                        uint64_t TBAAOffset,
                                        bool isNontemporal) {

  // Handle vectors differently to get better performance.
  if (Ty->isVectorType()) {
    llvm::Type *SrcTy = Value->getType();
    auto *VecTy = cast<llvm::VectorType>(SrcTy);
    // Handle vec3 special.
    if (VecTy->getNumElements() == 3) {
      // Our source is a vec3, do a shuffle vector to make it a vec4.
      llvm::Constant *Mask[] = {Builder.getInt32(0), Builder.getInt32(1),
                                Builder.getInt32(2),
                                llvm::UndefValue::get(Builder.getInt32Ty())};
      llvm::Value *MaskV = llvm::ConstantVector::get(Mask);
      Value = Builder.CreateShuffleVector(Value,
                                          llvm::UndefValue::get(VecTy),
                                          MaskV, "extractVec");
      SrcTy = llvm::VectorType::get(VecTy->getElementType(), 4);
    }
    if (Addr.getElementType() != SrcTy) {
      Addr = Builder.CreateElementBitCast(Addr, SrcTy, "storetmp");
    }
  }

  Value = EmitToMemory(Value, Ty);

  if (Ty->isAtomicType() ||
      (!isInit && typeIsSuitableForInlineAtomic(Ty, Volatile))) {
    EmitAtomicStore(RValue::get(Value),
                    LValue::MakeAddr(Addr, Ty, getContext(),
                                     AlignSource, TBAAInfo),
                    isInit);
    return;
  }

  llvm::StoreInst *Store = Builder.CreateStore(Value, Addr, Volatile);
  if (isNontemporal) {
    llvm::MDNode *Node =
        llvm::MDNode::get(Store->getContext(),
                          llvm::ConstantAsMetadata::get(Builder.getInt32(1)));
    Store->setMetadata(CGM.getModule().getMDKindID("nontemporal"), Node);
  }
  if (TBAAInfo) {
    llvm::MDNode *TBAAPath = CGM.getTBAAStructTagInfo(TBAABaseType, TBAAInfo,
                                                      TBAAOffset);
    if (TBAAPath)
      CGM.DecorateInstructionWithTBAA(Store, TBAAPath,
                                      false /*ConvertTypeToTag*/);
  }
}

void CodeGenFunction::EmitStoreOfScalar(llvm::Value *value, LValue lvalue,
                                        bool isInit) {
  EmitStoreOfScalar(value, lvalue.getAddress(), lvalue.isVolatile(),
                    lvalue.getType(), lvalue.getAlignmentSource(),
                    lvalue.getTBAAInfo(), isInit, lvalue.getTBAABaseType(),
                    lvalue.getTBAAOffset(), lvalue.isNontemporal());
}

/// EmitLoadOfLValue - Given an expression that represents a value lvalue, this
/// method emits the address of the lvalue, then loads the result as an rvalue,
/// returning the rvalue.
RValue CodeGenFunction::EmitLoadOfLValue(LValue LV, SourceLocation Loc) {
  if (LV.isObjCWeak()) {
    // load of a __weak object.
    Address AddrWeakObj = LV.getAddress();
    return RValue::get(CGM.getObjCRuntime().EmitObjCWeakRead(*this,
                                                             AddrWeakObj));
  }
  if (LV.getQuals().getObjCLifetime() == Qualifiers::OCL_Weak) {
    // In MRC mode, we do a load+autorelease.
    if (!getLangOpts().ObjCAutoRefCount) {
      return RValue::get(EmitARCLoadWeak(LV.getAddress()));
    }

    // In ARC mode, we load retained and then consume the value.
    llvm::Value *Object = EmitARCLoadWeakRetained(LV.getAddress());
    Object = EmitObjCConsumeObject(LV.getType(), Object);
    return RValue::get(Object);
  }

  if (LV.isSimple()) {
    assert(!LV.getType()->isFunctionType());

    // Everything needs a load.
    return RValue::get(EmitLoadOfScalar(LV, Loc));
  }

  if (LV.isVectorElt()) {
    llvm::LoadInst *Load = Builder.CreateLoad(LV.getVectorAddress(),
                                              LV.isVolatileQualified());
    return RValue::get(Builder.CreateExtractElement(Load, LV.getVectorIdx(),
                                                    "vecext"));
  }

  // If this is a reference to a subset of the elements of a vector, either
  // shuffle the input or extract/insert them as appropriate.
  if (LV.isExtVectorElt())
    return EmitLoadOfExtVectorElementLValue(LV);

  // Global Register variables always invoke intrinsics
  if (LV.isGlobalReg())
    return EmitLoadOfGlobalRegLValue(LV);

  assert(LV.isBitField() && "Unknown LValue type!");
  return EmitLoadOfBitfieldLValue(LV);
}

RValue CodeGenFunction::EmitLoadOfBitfieldLValue(LValue LV) {
  const CGBitFieldInfo &Info = LV.getBitFieldInfo();

  // Get the output type.
  llvm::Type *ResLTy = ConvertType(LV.getType());

  Address Ptr = LV.getBitFieldAddress();
  llvm::Value *Val = Builder.CreateLoad(Ptr, LV.isVolatileQualified(), "bf.load");

  if (Info.IsSigned) {
    assert(static_cast<unsigned>(Info.Offset + Info.Size) <= Info.StorageSize);
    unsigned HighBits = Info.StorageSize - Info.Offset - Info.Size;
    if (HighBits)
      Val = Builder.CreateShl(Val, HighBits, "bf.shl");
    if (Info.Offset + HighBits)
      Val = Builder.CreateAShr(Val, Info.Offset + HighBits, "bf.ashr");
  } else {
    if (Info.Offset)
      Val = Builder.CreateLShr(Val, Info.Offset, "bf.lshr");
    if (static_cast<unsigned>(Info.Offset) + Info.Size < Info.StorageSize)
      Val = Builder.CreateAnd(Val, llvm::APInt::getLowBitsSet(Info.StorageSize,
                                                              Info.Size),
                              "bf.clear");
  }
  Val = Builder.CreateIntCast(Val, ResLTy, Info.IsSigned, "bf.cast");

  return RValue::get(Val);
}

// If this is a reference to a subset of the elements of a vector, create an
// appropriate shufflevector.
RValue CodeGenFunction::EmitLoadOfExtVectorElementLValue(LValue LV) {
  llvm::Value *Vec = Builder.CreateLoad(LV.getExtVectorAddress(),
                                        LV.isVolatileQualified());

  const llvm::Constant *Elts = LV.getExtVectorElts();

  // If the result of the expression is a non-vector type, we must be extracting
  // a single element.  Just codegen as an extractelement.
  const VectorType *ExprVT = LV.getType()->getAs<VectorType>();
  if (!ExprVT) {
    unsigned InIdx = getAccessedFieldNo(0, Elts);
    llvm::Value *Elt = llvm::ConstantInt::get(SizeTy, InIdx);
    return RValue::get(Builder.CreateExtractElement(Vec, Elt));
  }

  // Always use shuffle vector to try to retain the original program structure
  unsigned NumResultElts = ExprVT->getNumElements();

  SmallVector<llvm::Constant*, 4> Mask;
  for (unsigned i = 0; i != NumResultElts; ++i)
    Mask.push_back(Builder.getInt32(getAccessedFieldNo(i, Elts)));

  llvm::Value *MaskV = llvm::ConstantVector::get(Mask);
  Vec = Builder.CreateShuffleVector(Vec, llvm::UndefValue::get(Vec->getType()),
                                    MaskV);
  return RValue::get(Vec);
}

/// @brief Generates lvalue for partial ext_vector access.
Address CodeGenFunction::EmitExtVectorElementLValue(LValue LV) {
  Address VectorAddress = LV.getExtVectorAddress();
  const VectorType *ExprVT = LV.getType()->getAs<VectorType>();
  QualType EQT = ExprVT->getElementType();
  llvm::Type *VectorElementTy = CGM.getTypes().ConvertType(EQT);
  
  Address CastToPointerElement =
    Builder.CreateElementBitCast(VectorAddress, VectorElementTy,
                                 "conv.ptr.element");
  
  const llvm::Constant *Elts = LV.getExtVectorElts();
  unsigned ix = getAccessedFieldNo(0, Elts);
  
  Address VectorBasePtrPlusIx =
    Builder.CreateConstInBoundsGEP(CastToPointerElement, ix,
                                   getContext().getTypeSizeInChars(EQT),
                                   "vector.elt");

  return VectorBasePtrPlusIx;
}

/// @brief Load of global gamed gegisters are always calls to intrinsics.
RValue CodeGenFunction::EmitLoadOfGlobalRegLValue(LValue LV) {
  assert((LV.getType()->isIntegerType() || LV.getType()->isPointerType()) &&
         "Bad type for register variable");
  llvm::MDNode *RegName = cast<llvm::MDNode>(
      cast<llvm::MetadataAsValue>(LV.getGlobalReg())->getMetadata());

  // We accept integer and pointer types only
  llvm::Type *OrigTy = CGM.getTypes().ConvertType(LV.getType());
  llvm::Type *Ty = OrigTy;
  if (OrigTy->isPointerTy())
    Ty = CGM.getTypes().getDataLayout().getIntPtrType(OrigTy);
  llvm::Type *Types[] = { Ty };

  llvm::Value *F = CGM.getIntrinsic(llvm::Intrinsic::read_register, Types);
  llvm::Value *Call = Builder.CreateCall(
      F, llvm::MetadataAsValue::get(Ty->getContext(), RegName));
  if (OrigTy->isPointerTy())
    Call = Builder.CreateIntToPtr(Call, OrigTy);
  return RValue::get(Call);
}


/// EmitStoreThroughLValue - Store the specified rvalue into the specified
/// lvalue, where both are guaranteed to the have the same type, and that type
/// is 'Ty'.
void CodeGenFunction::EmitStoreThroughLValue(RValue Src, LValue Dst,
                                             bool isInit) {
  if (!Dst.isSimple()) {
    if (Dst.isVectorElt()) {
      // Read/modify/write the vector, inserting the new element.
      llvm::Value *Vec = Builder.CreateLoad(Dst.getVectorAddress(),
                                            Dst.isVolatileQualified());
      Vec = Builder.CreateInsertElement(Vec, Src.getScalarVal(),
                                        Dst.getVectorIdx(), "vecins");
      Builder.CreateStore(Vec, Dst.getVectorAddress(),
                          Dst.isVolatileQualified());
      return;
    }

    // If this is an update of extended vector elements, insert them as
    // appropriate.
    if (Dst.isExtVectorElt())
      return EmitStoreThroughExtVectorComponentLValue(Src, Dst);

    if (Dst.isGlobalReg())
      return EmitStoreThroughGlobalRegLValue(Src, Dst);

    assert(Dst.isBitField() && "Unknown LValue type");
    return EmitStoreThroughBitfieldLValue(Src, Dst);
  }

  // There's special magic for assigning into an ARC-qualified l-value.
  if (Qualifiers::ObjCLifetime Lifetime = Dst.getQuals().getObjCLifetime()) {
    switch (Lifetime) {
    case Qualifiers::OCL_None:
      llvm_unreachable("present but none");

    case Qualifiers::OCL_ExplicitNone:
      // nothing special
      break;

    case Qualifiers::OCL_Strong:
      EmitARCStoreStrong(Dst, Src.getScalarVal(), /*ignore*/ true);
      return;

    case Qualifiers::OCL_Weak:
      EmitARCStoreWeak(Dst.getAddress(), Src.getScalarVal(), /*ignore*/ true);
      return;

    case Qualifiers::OCL_Autoreleasing:
      Src = RValue::get(EmitObjCExtendObjectLifetime(Dst.getType(),
                                                     Src.getScalarVal()));
      // fall into the normal path
      break;
    }
  }

  if (Dst.isObjCWeak() && !Dst.isNonGC()) {
    // load of a __weak object.
    Address LvalueDst = Dst.getAddress();
    llvm::Value *src = Src.getScalarVal();
     CGM.getObjCRuntime().EmitObjCWeakAssign(*this, src, LvalueDst);
    return;
  }

  if (Dst.isObjCStrong() && !Dst.isNonGC()) {
    // load of a __strong object.
    Address LvalueDst = Dst.getAddress();
    llvm::Value *src = Src.getScalarVal();
    if (Dst.isObjCIvar()) {
      assert(Dst.getBaseIvarExp() && "BaseIvarExp is NULL");
      llvm::Type *ResultType = IntPtrTy;
      Address dst = EmitPointerWithAlignment(Dst.getBaseIvarExp());
      llvm::Value *RHS = dst.getPointer();
      RHS = Builder.CreatePtrToInt(RHS, ResultType, "sub.ptr.rhs.cast");
      llvm::Value *LHS =
        Builder.CreatePtrToInt(LvalueDst.getPointer(), ResultType,
                               "sub.ptr.lhs.cast");
      llvm::Value *BytesBetween = Builder.CreateSub(LHS, RHS, "ivar.offset");
      CGM.getObjCRuntime().EmitObjCIvarAssign(*this, src, dst,
                                              BytesBetween);
    } else if (Dst.isGlobalObjCRef()) {
      CGM.getObjCRuntime().EmitObjCGlobalAssign(*this, src, LvalueDst,
                                                Dst.isThreadLocalRef());
    }
    else
      CGM.getObjCRuntime().EmitObjCStrongCastAssign(*this, src, LvalueDst);
    return;
  }

  assert(Src.isScalar() && "Can't emit an agg store with this method");
  EmitStoreOfScalar(Src.getScalarVal(), Dst, isInit);
}

void CodeGenFunction::EmitStoreThroughBitfieldLValue(RValue Src, LValue Dst,
                                                     llvm::Value **Result) {
  const CGBitFieldInfo &Info = Dst.getBitFieldInfo();
  llvm::Type *ResLTy = ConvertTypeForMem(Dst.getType());
  Address Ptr = Dst.getBitFieldAddress();

  // Get the source value, truncated to the width of the bit-field.
  llvm::Value *SrcVal = Src.getScalarVal();

  // Cast the source to the storage type and shift it into place.
  SrcVal = Builder.CreateIntCast(SrcVal, Ptr.getElementType(),
                                 /*IsSigned=*/false);
  llvm::Value *MaskedVal = SrcVal;

  // See if there are other bits in the bitfield's storage we'll need to load
  // and mask together with source before storing.
  if (Info.StorageSize != Info.Size) {
    assert(Info.StorageSize > Info.Size && "Invalid bitfield size.");
    llvm::Value *Val =
      Builder.CreateLoad(Ptr, Dst.isVolatileQualified(), "bf.load");

    // Mask the source value as needed.
    if (!hasBooleanRepresentation(Dst.getType()))
      SrcVal = Builder.CreateAnd(SrcVal,
                                 llvm::APInt::getLowBitsSet(Info.StorageSize,
                                                            Info.Size),
                                 "bf.value");
    MaskedVal = SrcVal;
    if (Info.Offset)
      SrcVal = Builder.CreateShl(SrcVal, Info.Offset, "bf.shl");

    // Mask out the original value.
    Val = Builder.CreateAnd(Val,
                            ~llvm::APInt::getBitsSet(Info.StorageSize,
                                                     Info.Offset,
                                                     Info.Offset + Info.Size),
                            "bf.clear");

    // Or together the unchanged values and the source value.
    SrcVal = Builder.CreateOr(Val, SrcVal, "bf.set");
  } else {
    assert(Info.Offset == 0);
  }

  // Write the new value back out.
  Builder.CreateStore(SrcVal, Ptr, Dst.isVolatileQualified());

  // Return the new value of the bit-field, if requested.
  if (Result) {
    llvm::Value *ResultVal = MaskedVal;

    // Sign extend the value if needed.
    if (Info.IsSigned) {
      assert(Info.Size <= Info.StorageSize);
      unsigned HighBits = Info.StorageSize - Info.Size;
      if (HighBits) {
        ResultVal = Builder.CreateShl(ResultVal, HighBits, "bf.result.shl");
        ResultVal = Builder.CreateAShr(ResultVal, HighBits, "bf.result.ashr");
      }
    }

    ResultVal = Builder.CreateIntCast(ResultVal, ResLTy, Info.IsSigned,
                                      "bf.result.cast");
    *Result = EmitFromMemory(ResultVal, Dst.getType());
  }
}

void CodeGenFunction::EmitStoreThroughExtVectorComponentLValue(RValue Src,
                                                               LValue Dst) {
  // This access turns into a read/modify/write of the vector.  Load the input
  // value now.
  llvm::Value *Vec = Builder.CreateLoad(Dst.getExtVectorAddress(),
                                        Dst.isVolatileQualified());
  const llvm::Constant *Elts = Dst.getExtVectorElts();

  llvm::Value *SrcVal = Src.getScalarVal();

  if (const VectorType *VTy = Dst.getType()->getAs<VectorType>()) {
    unsigned NumSrcElts = VTy->getNumElements();
    unsigned NumDstElts =
       cast<llvm::VectorType>(Vec->getType())->getNumElements();
    if (NumDstElts == NumSrcElts) {
      // Use shuffle vector is the src and destination are the same number of
      // elements and restore the vector mask since it is on the side it will be
      // stored.
      SmallVector<llvm::Constant*, 4> Mask(NumDstElts);
      for (unsigned i = 0; i != NumSrcElts; ++i)
        Mask[getAccessedFieldNo(i, Elts)] = Builder.getInt32(i);

      llvm::Value *MaskV = llvm::ConstantVector::get(Mask);
      Vec = Builder.CreateShuffleVector(SrcVal,
                                        llvm::UndefValue::get(Vec->getType()),
                                        MaskV);
    } else if (NumDstElts > NumSrcElts) {
      // Extended the source vector to the same length and then shuffle it
      // into the destination.
      // FIXME: since we're shuffling with undef, can we just use the indices
      //        into that?  This could be simpler.
      SmallVector<llvm::Constant*, 4> ExtMask;
      for (unsigned i = 0; i != NumSrcElts; ++i)
        ExtMask.push_back(Builder.getInt32(i));
      ExtMask.resize(NumDstElts, llvm::UndefValue::get(Int32Ty));
      llvm::Value *ExtMaskV = llvm::ConstantVector::get(ExtMask);
      llvm::Value *ExtSrcVal =
        Builder.CreateShuffleVector(SrcVal,
                                    llvm::UndefValue::get(SrcVal->getType()),
                                    ExtMaskV);
      // build identity
      SmallVector<llvm::Constant*, 4> Mask;
      for (unsigned i = 0; i != NumDstElts; ++i)
        Mask.push_back(Builder.getInt32(i));

      // When the vector size is odd and .odd or .hi is used, the last element
      // of the Elts constant array will be one past the size of the vector.
      // Ignore the last element here, if it is greater than the mask size.
      if (getAccessedFieldNo(NumSrcElts - 1, Elts) == Mask.size())
        NumSrcElts--;

      // modify when what gets shuffled in
      for (unsigned i = 0; i != NumSrcElts; ++i)
        Mask[getAccessedFieldNo(i, Elts)] = Builder.getInt32(i+NumDstElts);
      llvm::Value *MaskV = llvm::ConstantVector::get(Mask);
      Vec = Builder.CreateShuffleVector(Vec, ExtSrcVal, MaskV);
    } else {
      // We should never shorten the vector
      llvm_unreachable("unexpected shorten vector length");
    }
  } else {
    // If the Src is a scalar (not a vector) it must be updating one element.
    unsigned InIdx = getAccessedFieldNo(0, Elts);
    llvm::Value *Elt = llvm::ConstantInt::get(SizeTy, InIdx);
    Vec = Builder.CreateInsertElement(Vec, SrcVal, Elt);
  }

  Builder.CreateStore(Vec, Dst.getExtVectorAddress(),
                      Dst.isVolatileQualified());
}

/// @brief Store of global named registers are always calls to intrinsics.
void CodeGenFunction::EmitStoreThroughGlobalRegLValue(RValue Src, LValue Dst) {
  assert((Dst.getType()->isIntegerType() || Dst.getType()->isPointerType()) &&
         "Bad type for register variable");
  llvm::MDNode *RegName = cast<llvm::MDNode>(
      cast<llvm::MetadataAsValue>(Dst.getGlobalReg())->getMetadata());
  assert(RegName && "Register LValue is not metadata");

  // We accept integer and pointer types only
  llvm::Type *OrigTy = CGM.getTypes().ConvertType(Dst.getType());
  llvm::Type *Ty = OrigTy;
  if (OrigTy->isPointerTy())
    Ty = CGM.getTypes().getDataLayout().getIntPtrType(OrigTy);
  llvm::Type *Types[] = { Ty };

  llvm::Value *F = CGM.getIntrinsic(llvm::Intrinsic::write_register, Types);
  llvm::Value *Value = Src.getScalarVal();
  if (OrigTy->isPointerTy())
    Value = Builder.CreatePtrToInt(Value, Ty);
  Builder.CreateCall(
      F, {llvm::MetadataAsValue::get(Ty->getContext(), RegName), Value});
}

// setObjCGCLValueClass - sets class of the lvalue for the purpose of
// generating write-barries API. It is currently a global, ivar,
// or neither.
static void setObjCGCLValueClass(const ASTContext &Ctx, const Expr *E,
                                 LValue &LV,
                                 bool IsMemberAccess=false) {
  if (Ctx.getLangOpts().getGC() == LangOptions::NonGC)
    return;

  if (isa<ObjCIvarRefExpr>(E)) {
    QualType ExpTy = E->getType();
    if (IsMemberAccess && ExpTy->isPointerType()) {
      // If ivar is a structure pointer, assigning to field of
      // this struct follows gcc's behavior and makes it a non-ivar
      // writer-barrier conservatively.
      ExpTy = ExpTy->getAs<PointerType>()->getPointeeType();
      if (ExpTy->isRecordType()) {
        LV.setObjCIvar(false);
        return;
      }
    }
    LV.setObjCIvar(true);
    auto *Exp = cast<ObjCIvarRefExpr>(const_cast<Expr *>(E));
    LV.setBaseIvarExp(Exp->getBase());
    LV.setObjCArray(E->getType()->isArrayType());
    return;
  }

  if (const auto *Exp = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast<VarDecl>(Exp->getDecl())) {
      if (VD->hasGlobalStorage()) {
        LV.setGlobalObjCRef(true);
        LV.setThreadLocalRef(VD->getTLSKind() != VarDecl::TLS_None);
      }
    }
    LV.setObjCArray(E->getType()->isArrayType());
    return;
  }

  if (const auto *Exp = dyn_cast<UnaryOperator>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getSubExpr(), LV, IsMemberAccess);
    return;
  }

  if (const auto *Exp = dyn_cast<ParenExpr>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getSubExpr(), LV, IsMemberAccess);
    if (LV.isObjCIvar()) {
      // If cast is to a structure pointer, follow gcc's behavior and make it
      // a non-ivar write-barrier.
      QualType ExpTy = E->getType();
      if (ExpTy->isPointerType())
        ExpTy = ExpTy->getAs<PointerType>()->getPointeeType();
      if (ExpTy->isRecordType())
        LV.setObjCIvar(false);
    }
    return;
  }

  if (const auto *Exp = dyn_cast<GenericSelectionExpr>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getResultExpr(), LV);
    return;
  }

  if (const auto *Exp = dyn_cast<ImplicitCastExpr>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getSubExpr(), LV, IsMemberAccess);
    return;
  }

  if (const auto *Exp = dyn_cast<CStyleCastExpr>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getSubExpr(), LV, IsMemberAccess);
    return;
  }

  if (const auto *Exp = dyn_cast<ObjCBridgedCastExpr>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getSubExpr(), LV, IsMemberAccess);
    return;
  }

  if (const auto *Exp = dyn_cast<ArraySubscriptExpr>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getBase(), LV);
    if (LV.isObjCIvar() && !LV.isObjCArray())
      // Using array syntax to assigning to what an ivar points to is not
      // same as assigning to the ivar itself. {id *Names;} Names[i] = 0;
      LV.setObjCIvar(false);
    else if (LV.isGlobalObjCRef() && !LV.isObjCArray())
      // Using array syntax to assigning to what global points to is not
      // same as assigning to the global itself. {id *G;} G[i] = 0;
      LV.setGlobalObjCRef(false);
    return;
  }

  if (const auto *Exp = dyn_cast<MemberExpr>(E)) {
    setObjCGCLValueClass(Ctx, Exp->getBase(), LV, true);
    // We don't know if member is an 'ivar', but this flag is looked at
    // only in the context of LV.isObjCIvar().
    LV.setObjCArray(E->getType()->isArrayType());
    return;
  }
}

static llvm::Value *
EmitBitCastOfLValueToProperType(CodeGenFunction &CGF,
                                llvm::Value *V, llvm::Type *IRType,
                                StringRef Name = StringRef()) {
  unsigned AS = cast<llvm::PointerType>(V->getType())->getAddressSpace();
  return CGF.Builder.CreateBitCast(V, IRType->getPointerTo(AS), Name);
}

static LValue EmitThreadPrivateVarDeclLValue(
    CodeGenFunction &CGF, const VarDecl *VD, QualType T, Address Addr,
    llvm::Type *RealVarTy, SourceLocation Loc) {
  Addr = CGF.CGM.getOpenMPRuntime().getAddrOfThreadPrivate(CGF, VD, Addr, Loc);
  Addr = CGF.Builder.CreateElementBitCast(Addr, RealVarTy);
  return CGF.MakeAddrLValue(Addr, T, AlignmentSource::Decl);
}

Address CodeGenFunction::EmitLoadOfReference(Address Addr,
                                             const ReferenceType *RefTy,
                                             AlignmentSource *Source) {
  llvm::Value *Ptr = Builder.CreateLoad(Addr);
  return Address(Ptr, getNaturalTypeAlignment(RefTy->getPointeeType(),
                                              Source, /*forPointee*/ true));
  
}

LValue CodeGenFunction::EmitLoadOfReferenceLValue(Address RefAddr,
                                                  const ReferenceType *RefTy) {
  AlignmentSource Source;
  Address Addr = EmitLoadOfReference(RefAddr, RefTy, &Source);
  return MakeAddrLValue(Addr, RefTy->getPointeeType(), Source);
}

Address CodeGenFunction::EmitLoadOfPointer(Address Ptr,
                                           const PointerType *PtrTy,
                                           AlignmentSource *Source) {
  llvm::Value *Addr = Builder.CreateLoad(Ptr);
  return Address(Addr, getNaturalTypeAlignment(PtrTy->getPointeeType(), Source,
                                               /*forPointeeType=*/true));
}

LValue CodeGenFunction::EmitLoadOfPointerLValue(Address PtrAddr,
                                                const PointerType *PtrTy) {
  AlignmentSource Source;
  Address Addr = EmitLoadOfPointer(PtrAddr, PtrTy, &Source);
  return MakeAddrLValue(Addr, PtrTy->getPointeeType(), Source);
}

static LValue EmitGlobalVarDeclLValue(CodeGenFunction &CGF,
                                      const Expr *E, const VarDecl *VD) {
  QualType T = E->getType();

  // If it's thread_local, emit a call to its wrapper function instead.
  if (VD->getTLSKind() == VarDecl::TLS_Dynamic &&
      CGF.CGM.getCXXABI().usesThreadWrapperFunction())
    return CGF.CGM.getCXXABI().EmitThreadLocalVarDeclLValue(CGF, VD, T);

  llvm::Value *V = CGF.CGM.GetAddrOfGlobalVar(VD);
  llvm::Type *RealVarTy = CGF.getTypes().ConvertTypeForMem(VD->getType());
  V = EmitBitCastOfLValueToProperType(CGF, V, RealVarTy);
  CharUnits Alignment = CGF.getContext().getDeclAlign(VD);
  Address Addr(V, Alignment);
  LValue LV;
  // Emit reference to the private copy of the variable if it is an OpenMP
  // threadprivate variable.
  if (CGF.getLangOpts().OpenMP && VD->hasAttr<OMPThreadPrivateDeclAttr>())
    return EmitThreadPrivateVarDeclLValue(CGF, VD, T, Addr, RealVarTy,
                                          E->getExprLoc());
  if (auto RefTy = VD->getType()->getAs<ReferenceType>()) {
    LV = CGF.EmitLoadOfReferenceLValue(Addr, RefTy);
  } else {
    LV = CGF.MakeAddrLValue(Addr, T, AlignmentSource::Decl);
  }
  setObjCGCLValueClass(CGF.getContext(), E, LV);
  return LV;
}

static LValue EmitFunctionDeclLValue(CodeGenFunction &CGF,
                                     const Expr *E, const FunctionDecl *FD) {
  llvm::Value *V = CGF.CGM.GetAddrOfFunction(FD);
  if (!FD->hasPrototype()) {
    if (const FunctionProtoType *Proto =
            FD->getType()->getAs<FunctionProtoType>()) {
      // Ugly case: for a K&R-style definition, the type of the definition
      // isn't the same as the type of a use.  Correct for this with a
      // bitcast.
      QualType NoProtoType =
          CGF.getContext().getFunctionNoProtoType(Proto->getReturnType());
      NoProtoType = CGF.getContext().getPointerType(NoProtoType);
      V = CGF.Builder.CreateBitCast(V, CGF.ConvertType(NoProtoType));
    }
  }
  CharUnits Alignment = CGF.getContext().getDeclAlign(FD);
  return CGF.MakeAddrLValue(V, E->getType(), Alignment, AlignmentSource::Decl);
}

static LValue EmitCapturedFieldLValue(CodeGenFunction &CGF, const FieldDecl *FD,
                                      llvm::Value *ThisValue) {
  QualType TagType = CGF.getContext().getTagDeclType(FD->getParent());
  LValue LV = CGF.MakeNaturalAlignAddrLValue(ThisValue, TagType);
  return CGF.EmitLValueForField(LV, FD);
}

/// Named Registers are named metadata pointing to the register name
/// which will be read from/written to as an argument to the intrinsic
/// @llvm.read/write_register.
/// So far, only the name is being passed down, but other options such as
/// register type, allocation type or even optimization options could be
/// passed down via the metadata node.
static LValue EmitGlobalNamedRegister(const VarDecl *VD, CodeGenModule &CGM) {
  SmallString<64> Name("llvm.named.register.");
  AsmLabelAttr *Asm = VD->getAttr<AsmLabelAttr>();
  assert(Asm->getLabel().size() < 64-Name.size() &&
      "Register name too big");
  Name.append(Asm->getLabel());
  llvm::NamedMDNode *M =
    CGM.getModule().getOrInsertNamedMetadata(Name);
  if (M->getNumOperands() == 0) {
    llvm::MDString *Str = llvm::MDString::get(CGM.getLLVMContext(),
                                              Asm->getLabel());
    llvm::Metadata *Ops[] = {Str};
    M->addOperand(llvm::MDNode::get(CGM.getLLVMContext(), Ops));
  }

  CharUnits Alignment = CGM.getContext().getDeclAlign(VD);

  llvm::Value *Ptr =
    llvm::MetadataAsValue::get(CGM.getLLVMContext(), M->getOperand(0));
  return LValue::MakeGlobalReg(Address(Ptr, Alignment), VD->getType());
}

LValue CodeGenFunction::EmitDeclRefLValue(const DeclRefExpr *E) {
  const NamedDecl *ND = E->getDecl();
  QualType T = E->getType();

  if (const auto *VD = dyn_cast<VarDecl>(ND)) {
    // Global Named registers access via intrinsics only
    if (VD->getStorageClass() == SC_Register &&
        VD->hasAttr<AsmLabelAttr>() && !VD->isLocalVarDecl())
      return EmitGlobalNamedRegister(VD, CGM);

    // A DeclRefExpr for a reference initialized by a constant expression can
    // appear without being odr-used. Directly emit the constant initializer.
    const Expr *Init = VD->getAnyInitializer(VD);
    if (Init && !isa<ParmVarDecl>(VD) && VD->getType()->isReferenceType() &&
        VD->isUsableInConstantExpressions(getContext()) &&
        VD->checkInitIsICE() &&
        // Do not emit if it is private OpenMP variable.
        !(E->refersToEnclosingVariableOrCapture() && CapturedStmtInfo &&
          LocalDeclMap.count(VD))) {
      llvm::Constant *Val =
        CGM.EmitConstantValue(*VD->evaluateValue(), VD->getType(), this);
      assert(Val && "failed to emit reference constant expression");
      // FIXME: Eventually we will want to emit vector element references.

      // Should we be using the alignment of the constant pointer we emitted?
      CharUnits Alignment = getNaturalTypeAlignment(E->getType(), nullptr,
                                                    /*pointee*/ true);

      return MakeAddrLValue(Address(Val, Alignment), T, AlignmentSource::Decl);
    }

    // Check for captured variables.
    if (E->refersToEnclosingVariableOrCapture()) {
      if (auto *FD = LambdaCaptureFields.lookup(VD))
        return EmitCapturedFieldLValue(*this, FD, CXXABIThisValue);
      else if (CapturedStmtInfo) {
        auto it = LocalDeclMap.find(VD);
        if (it != LocalDeclMap.end()) {
          if (auto RefTy = VD->getType()->getAs<ReferenceType>()) {
            return EmitLoadOfReferenceLValue(it->second, RefTy);
          }
          return MakeAddrLValue(it->second, T);
        }
        LValue CapLVal =
            EmitCapturedFieldLValue(*this, CapturedStmtInfo->lookup(VD),
                                    CapturedStmtInfo->getContextValue());
        return MakeAddrLValue(
            Address(CapLVal.getPointer(), getContext().getDeclAlign(VD)),
            CapLVal.getType(), AlignmentSource::Decl);
      }

      assert(isa<BlockDecl>(CurCodeDecl));
      Address addr = GetAddrOfBlockDecl(VD, VD->hasAttr<BlocksAttr>());
      return MakeAddrLValue(addr, T, AlignmentSource::Decl);
    }
  }

  // FIXME: We should be able to assert this for FunctionDecls as well!
  // FIXME: We should be able to assert this for all DeclRefExprs, not just
  // those with a valid source location.
  assert((ND->isUsed(false) || !isa<VarDecl>(ND) ||
          !E->getLocation().isValid()) &&
         "Should not use decl without marking it used!");

  if (ND->hasAttr<WeakRefAttr>()) {
    const auto *VD = cast<ValueDecl>(ND);
    ConstantAddress Aliasee = CGM.GetWeakRefReference(VD);
    return MakeAddrLValue(Aliasee, T, AlignmentSource::Decl);
  }

  if (const auto *VD = dyn_cast<VarDecl>(ND)) {
    // Check if this is a global variable.
    if (VD->hasLinkage() || VD->isStaticDataMember())
      return EmitGlobalVarDeclLValue(*this, E, VD);

    Address addr = Address::invalid();

    // The variable should generally be present in the local decl map.
    auto iter = LocalDeclMap.find(VD);
    if (iter != LocalDeclMap.end()) {
      addr = iter->second;

    // Otherwise, it might be static local we haven't emitted yet for
    // some reason; most likely, because it's in an outer function.
    } else if (VD->isStaticLocal()) {
      addr = Address(CGM.getOrCreateStaticVarDecl(
          *VD, CGM.getLLVMLinkageVarDefinition(VD, /*isConstant=*/false)),
                     getContext().getDeclAlign(VD));

    // No other cases for now.
    } else {
      llvm_unreachable("DeclRefExpr for Decl not entered in LocalDeclMap?");
    }


    // Check for OpenMP threadprivate variables.
    if (getLangOpts().OpenMP && VD->hasAttr<OMPThreadPrivateDeclAttr>()) {
      return EmitThreadPrivateVarDeclLValue(
          *this, VD, T, addr, getTypes().ConvertTypeForMem(VD->getType()),
          E->getExprLoc());
    }

    // Drill into block byref variables.
    bool isBlockByref = VD->hasAttr<BlocksAttr>();
    if (isBlockByref) {
      addr = emitBlockByrefAddress(addr, VD);
    }

    // Drill into reference types.
    LValue LV;
    if (auto RefTy = VD->getType()->getAs<ReferenceType>()) {
      LV = EmitLoadOfReferenceLValue(addr, RefTy);
    } else {
      LV = MakeAddrLValue(addr, T, AlignmentSource::Decl);
    }

    bool isLocalStorage = VD->hasLocalStorage();

    bool NonGCable = isLocalStorage &&
                     !VD->getType()->isReferenceType() &&
                     !isBlockByref;
    if (NonGCable) {
      LV.getQuals().removeObjCGCAttr();
      LV.setNonGC(true);
    }

    bool isImpreciseLifetime =
      (isLocalStorage && !VD->hasAttr<ObjCPreciseLifetimeAttr>());
    if (isImpreciseLifetime)
      LV.setARCPreciseLifetime(ARCImpreciseLifetime);
    setObjCGCLValueClass(getContext(), E, LV);
    return LV;
  }

  if (const auto *FD = dyn_cast<FunctionDecl>(ND))
    return EmitFunctionDeclLValue(*this, E, FD);

  llvm_unreachable("Unhandled DeclRefExpr");
}

LValue CodeGenFunction::EmitUnaryOpLValue(const UnaryOperator *E) {
  // __extension__ doesn't affect lvalue-ness.
  if (E->getOpcode() == UO_Extension)
    return EmitLValue(E->getSubExpr());

  QualType ExprTy = getContext().getCanonicalType(E->getSubExpr()->getType());
  switch (E->getOpcode()) {
  default: llvm_unreachable("Unknown unary operator lvalue!");
  case UO_Deref: {
    QualType T = E->getSubExpr()->getType()->getPointeeType();
    assert(!T.isNull() && "CodeGenFunction::EmitUnaryOpLValue: Illegal type");

    AlignmentSource AlignSource;
    Address Addr = EmitPointerWithAlignment(E->getSubExpr(), &AlignSource);
    LValue LV = MakeAddrLValue(Addr, T, AlignSource);
    LV.getQuals().setAddressSpace(ExprTy.getAddressSpace());

    // We should not generate __weak write barrier on indirect reference
    // of a pointer to object; as in void foo (__weak id *param); *param = 0;
    // But, we continue to generate __strong write barrier on indirect write
    // into a pointer to object.
    if (getLangOpts().ObjC1 &&
        getLangOpts().getGC() != LangOptions::NonGC &&
        LV.isObjCWeak())
      LV.setNonGC(!E->isOBJCGCCandidate(getContext()));
    return LV;
  }
  case UO_Real:
  case UO_Imag: {
    LValue LV = EmitLValue(E->getSubExpr());
    assert(LV.isSimple() && "real/imag on non-ordinary l-value");

    // __real is valid on scalars.  This is a faster way of testing that.
    // __imag can only produce an rvalue on scalars.
    if (E->getOpcode() == UO_Real &&
        !LV.getAddress().getElementType()->isStructTy()) {
      assert(E->getSubExpr()->getType()->isArithmeticType());
      return LV;
    }

    assert(E->getSubExpr()->getType()->isAnyComplexType());

    Address Component =
      (E->getOpcode() == UO_Real
         ? emitAddrOfRealComponent(LV.getAddress(), LV.getType())
         : emitAddrOfImagComponent(LV.getAddress(), LV.getType()));
    return MakeAddrLValue(Component, ExprTy, LV.getAlignmentSource());
  }
  case UO_PreInc:
  case UO_PreDec: {
    LValue LV = EmitLValue(E->getSubExpr());
    bool isInc = E->getOpcode() == UO_PreInc;

    if (E->getType()->isAnyComplexType())
      EmitComplexPrePostIncDec(E, LV, isInc, true/*isPre*/);
    else
      EmitScalarPrePostIncDec(E, LV, isInc, true/*isPre*/);
    return LV;
  }
  }
}

LValue CodeGenFunction::EmitStringLiteralLValue(const StringLiteral *E) {
  return MakeAddrLValue(CGM.GetAddrOfConstantStringFromLiteral(E),
                        E->getType(), AlignmentSource::Decl);
}

LValue CodeGenFunction::EmitObjCEncodeExprLValue(const ObjCEncodeExpr *E) {
  return MakeAddrLValue(CGM.GetAddrOfConstantStringFromObjCEncode(E),
                        E->getType(), AlignmentSource::Decl);
}

LValue CodeGenFunction::EmitPredefinedLValue(const PredefinedExpr *E) {
  auto SL = E->getFunctionName();
  assert(SL != nullptr && "No StringLiteral name in PredefinedExpr");
  StringRef FnName = CurFn->getName();
  if (FnName.startswith("\01"))
    FnName = FnName.substr(1);
  StringRef NameItems[] = {
      PredefinedExpr::getIdentTypeName(E->getIdentType()), FnName};
  std::string GVName = llvm::join(NameItems, NameItems + 2, ".");
  if (CurCodeDecl && isa<BlockDecl>(CurCodeDecl)) {
    auto C = CGM.GetAddrOfConstantCString(FnName, GVName.c_str());
    return MakeAddrLValue(C, E->getType(), AlignmentSource::Decl);
  }
  auto C = CGM.GetAddrOfConstantStringFromLiteral(SL, GVName);
  return MakeAddrLValue(C, E->getType(), AlignmentSource::Decl);
}

/// Emit a type description suitable for use by a runtime sanitizer library. The
/// format of a type descriptor is
///
/// \code
///   { i16 TypeKind, i16 TypeInfo }
/// \endcode
///
/// followed by an array of i8 containing the type name. TypeKind is 0 for an
/// integer, 1 for a floating point value, and -1 for anything else.
llvm::Constant *CodeGenFunction::EmitCheckTypeDescriptor(QualType T) {
  // Only emit each type's descriptor once.
  if (llvm::Constant *C = CGM.getTypeDescriptorFromMap(T))
    return C;

  uint16_t TypeKind = -1;
  uint16_t TypeInfo = 0;

  if (T->isIntegerType()) {
    TypeKind = 0;
    TypeInfo = (llvm::Log2_32(getContext().getTypeSize(T)) << 1) |
               (T->isSignedIntegerType() ? 1 : 0);
  } else if (T->isFloatingType()) {
    TypeKind = 1;
    TypeInfo = getContext().getTypeSize(T);
  }

  // Format the type name as if for a diagnostic, including quotes and
  // optionally an 'aka'.
  SmallString<32> Buffer;
  CGM.getDiags().ConvertArgToString(DiagnosticsEngine::ak_qualtype,
                                    (intptr_t)T.getAsOpaquePtr(),
                                    StringRef(), StringRef(), None, Buffer,
                                    None);

  llvm::Constant *Components[] = {
    Builder.getInt16(TypeKind), Builder.getInt16(TypeInfo),
    llvm::ConstantDataArray::getString(getLLVMContext(), Buffer)
  };
  llvm::Constant *Descriptor = llvm::ConstantStruct::getAnon(Components);

  auto *GV = new llvm::GlobalVariable(
      CGM.getModule(), Descriptor->getType(),
      /*isConstant=*/true, llvm::GlobalVariable::PrivateLinkage, Descriptor);
  GV->setUnnamedAddr(true);
  CGM.getSanitizerMetadata()->disableSanitizerForGlobal(GV);

  // Remember the descriptor for this type.
  CGM.setTypeDescriptorInMap(T, GV);

  return GV;
}

llvm::Value *CodeGenFunction::EmitCheckValue(llvm::Value *V) {
  llvm::Type *TargetTy = IntPtrTy;

  // Floating-point types which fit into intptr_t are bitcast to integers
  // and then passed directly (after zero-extension, if necessary).
  if (V->getType()->isFloatingPointTy()) {
    unsigned Bits = V->getType()->getPrimitiveSizeInBits();
    if (Bits <= TargetTy->getIntegerBitWidth())
      V = Builder.CreateBitCast(V, llvm::Type::getIntNTy(getLLVMContext(),
                                                         Bits));
  }

  // Integers which fit in intptr_t are zero-extended and passed directly.
  if (V->getType()->isIntegerTy() &&
      V->getType()->getIntegerBitWidth() <= TargetTy->getIntegerBitWidth())
    return Builder.CreateZExt(V, TargetTy);

  // Pointers are passed directly, everything else is passed by address.
  if (!V->getType()->isPointerTy()) {
    Address Ptr = CreateDefaultAlignTempAlloca(V->getType());
    Builder.CreateStore(V, Ptr);
    V = Ptr.getPointer();
  }
  return Builder.CreatePtrToInt(V, TargetTy);
}

/// \brief Emit a representation of a SourceLocation for passing to a handler
/// in a sanitizer runtime library. The format for this data is:
/// \code
///   struct SourceLocation {
///     const char *Filename;
///     int32_t Line, Column;
///   };
/// \endcode
/// For an invalid SourceLocation, the Filename pointer is null.
llvm::Constant *CodeGenFunction::EmitCheckSourceLocation(SourceLocation Loc) {
  llvm::Constant *Filename;
  int Line, Column;

  PresumedLoc PLoc = getContext().getSourceManager().getPresumedLoc(Loc);
  if (PLoc.isValid()) {
    auto FilenameGV = CGM.GetAddrOfConstantCString(PLoc.getFilename(), ".src");
    CGM.getSanitizerMetadata()->disableSanitizerForGlobal(
                          cast<llvm::GlobalVariable>(FilenameGV.getPointer()));
    Filename = FilenameGV.getPointer();
    Line = PLoc.getLine();
    Column = PLoc.getColumn();
  } else {
    Filename = llvm::Constant::getNullValue(Int8PtrTy);
    Line = Column = 0;
  }

  llvm::Constant *Data[] = {Filename, Builder.getInt32(Line),
                            Builder.getInt32(Column)};

  return llvm::ConstantStruct::getAnon(Data);
}

namespace {
/// \brief Specify under what conditions this check can be recovered
enum class CheckRecoverableKind {
  /// Always terminate program execution if this check fails.
  Unrecoverable,
  /// Check supports recovering, runtime has both fatal (noreturn) and
  /// non-fatal handlers for this check.
  Recoverable,
  /// Runtime conditionally aborts, always need to support recovery.
  AlwaysRecoverable
};
}

static CheckRecoverableKind getRecoverableKind(SanitizerMask Kind) {
  assert(llvm::countPopulation(Kind) == 1);
  switch (Kind) {
  case SanitizerKind::Vptr:
    return CheckRecoverableKind::AlwaysRecoverable;
  case SanitizerKind::Return:
  case SanitizerKind::Unreachable:
    return CheckRecoverableKind::Unrecoverable;
  default:
    return CheckRecoverableKind::Recoverable;
  }
}

static void emitCheckHandlerCall(CodeGenFunction &CGF,
                                 llvm::FunctionType *FnType,
                                 ArrayRef<llvm::Value *> FnArgs,
                                 StringRef CheckName,
                                 CheckRecoverableKind RecoverKind, bool IsFatal,
                                 llvm::BasicBlock *ContBB) {
  assert(IsFatal || RecoverKind != CheckRecoverableKind::Unrecoverable);
  bool NeedsAbortSuffix =
      IsFatal && RecoverKind != CheckRecoverableKind::Unrecoverable;
  std::string FnName = ("__ubsan_handle_" + CheckName +
                        (NeedsAbortSuffix ? "_abort" : "")).str();
  bool MayReturn =
      !IsFatal || RecoverKind == CheckRecoverableKind::AlwaysRecoverable;

  llvm::AttrBuilder B;
  if (!MayReturn) {
    B.addAttribute(llvm::Attribute::NoReturn)
        .addAttribute(llvm::Attribute::NoUnwind);
  }
  B.addAttribute(llvm::Attribute::UWTable);

  llvm::Value *Fn = CGF.CGM.CreateRuntimeFunction(
      FnType, FnName,
      llvm::AttributeSet::get(CGF.getLLVMContext(),
                              llvm::AttributeSet::FunctionIndex, B));
  llvm::CallInst *HandlerCall = CGF.EmitNounwindRuntimeCall(Fn, FnArgs);
  if (!MayReturn) {
    HandlerCall->setDoesNotReturn();
    CGF.Builder.CreateUnreachable();
  } else {
    CGF.Builder.CreateBr(ContBB);
  }
}

/**
Paul: generic function for emiting a check.
See EmitNounwindRuntimeCall*/
void CodeGenFunction::HexEmitCheck(StringRef FunName,
                                   ArrayRef<llvm::Value *> DynamicArgs,
                                   llvm::Value *DstTyHashValue,
                                   llvm::Value *PointerTyHashValue) {
  bool blacklisted =
    CGM.getContext().getSanitizerBlacklist().isBlacklistedFunction(
      CurFn->getName()) ? 1 : 0;
  if (blacklisted) return;

  SmallVector<llvm::Value *, 6> Args;
  SmallVector<llvm::Type *, 6> ArgTypes;
  Args.reserve(DynamicArgs.size() + 4);
  ArgTypes.reserve(DynamicArgs.size() + 4);

  for (size_t i = 0, n = DynamicArgs.size(); i != n; ++i) {
    Args.push_back(EmitCheckValue(DynamicArgs[i]));
    ArgTypes.push_back(IntPtrTy);
  }
  Args.push_back(DstTyHashValue);
  ArgTypes.push_back(Builder.getInt64Ty());

  Args.push_back(PointerTyHashValue);
  ArgTypes.push_back(Builder.getInt64Ty());

  llvm::Value * const_null = llvm::ConstantInt::getNullValue(Builder.getInt64Ty());

  Args.push_back(const_null);
  ArgTypes.push_back(Builder.getInt64Ty());

  llvm::FunctionType *FnType =
    llvm::FunctionType::get(CGM.VoidTy, ArgTypes, false);

  llvm::AttrBuilder B;
  B.addAttribute(llvm::Attribute::UWTable);

  llvm::Value *Fn = CGM.CreateRuntimeFunction(
    FnType, FunName,
    llvm::AttributeSet::get(getLLVMContext(),
                            llvm::AttributeSet::FunctionIndex, B));
  

  /*llvm::Value * CheckIntrinsic;
  if (DynamicArgs.size() == 1)
	  CheckIntrinsic = Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::cast_san_check), {EmitCheckValue(DynamicArgs[0]), DstTyHashValue, PointerTyHashValue});
  else if (DynamicArgs.size() == 2)
	  CheckIntrinsic = Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::cast_san_check_changing), {EmitCheckValue(DynamicArgs[0]),EmitCheckValue(DynamicArgs[1]), DstTyHashValue, PointerTyHashValue});
  else
  assert (false && "Unexpected: more than 2 args to cast");*/

  EmitNounwindRuntimeCall(Fn, Args);
}


void CodeGenFunction::EmitCheck(
    ArrayRef<std::pair<llvm::Value *, SanitizerMask>> Checked,
    StringRef CheckName, ArrayRef<llvm::Constant *> StaticArgs,
    ArrayRef<llvm::Value *> DynamicArgs) {
  assert(IsSanitizerScope);
  assert(Checked.size() > 0);

  llvm::Value *FatalCond = nullptr;
  llvm::Value *RecoverableCond = nullptr;
  llvm::Value *TrapCond = nullptr;
  for (int i = 0, n = Checked.size(); i < n; ++i) {
    llvm::Value *Check = Checked[i].first;
    // -fsanitize-trap= overrides -fsanitize-recover=.
    llvm::Value *&Cond =
        CGM.getCodeGenOpts().SanitizeTrap.has(Checked[i].second)
            ? TrapCond
            : CGM.getCodeGenOpts().SanitizeRecover.has(Checked[i].second)
                  ? RecoverableCond
                  : FatalCond;
    Cond = Cond ? Builder.CreateAnd(Cond, Check) : Check;
  }

  if (TrapCond)
    EmitTrapCheck(TrapCond);
  if (!FatalCond && !RecoverableCond)
    return;

  llvm::Value *JointCond;
  if (FatalCond && RecoverableCond)
    JointCond = Builder.CreateAnd(FatalCond, RecoverableCond);
  else
    JointCond = FatalCond ? FatalCond : RecoverableCond;
  assert(JointCond);

  CheckRecoverableKind RecoverKind = getRecoverableKind(Checked[0].second);
  // In cross-DSO CFI mode this code is used to generate __cfi_check_fail, which
  // includes all checks, even those that are not in SanOpts.
  assert(CGM.getCodeGenOpts().SanitizeCfiCrossDso ||
         SanOpts.has(Checked[0].second));
#ifndef NDEBUG
  for (int i = 1, n = Checked.size(); i < n; ++i) {
    assert(RecoverKind == getRecoverableKind(Checked[i].second) &&
           "All recoverable kinds in a single check must be same!");
    assert(CGM.getCodeGenOpts().SanitizeCfiCrossDso ||
           SanOpts.has(Checked[i].second));
  }
#endif

  llvm::BasicBlock *Cont = createBasicBlock("cont");
  llvm::BasicBlock *Handlers = createBasicBlock("handler." + CheckName);
  llvm::Instruction *Branch = Builder.CreateCondBr(JointCond, Cont, Handlers);
  // Give hint that we very much don't expect to execute the handler
  // Value chosen to match UR_NONTAKEN_WEIGHT, see BranchProbabilityInfo.cpp
  llvm::MDBuilder MDHelper(getLLVMContext());
  llvm::MDNode *Node = MDHelper.createBranchWeights((1U << 20) - 1, 1);
  Branch->setMetadata(llvm::LLVMContext::MD_prof, Node);
  EmitBlock(Handlers);

  // Handler functions take an i8* pointing to the (handler-specific) static
  // information block, followed by a sequence of intptr_t arguments
  // representing operand values.
  SmallVector<llvm::Value *, 4> Args;
  SmallVector<llvm::Type *, 4> ArgTypes;
  Args.reserve(DynamicArgs.size() + 1);
  ArgTypes.reserve(DynamicArgs.size() + 1);

  // Emit handler arguments and create handler function type.
  if (!StaticArgs.empty()) {
    llvm::Constant *Info = llvm::ConstantStruct::getAnon(StaticArgs);
    auto *InfoPtr =
        new llvm::GlobalVariable(CGM.getModule(), Info->getType(), false,
                                 llvm::GlobalVariable::PrivateLinkage, Info);
    InfoPtr->setUnnamedAddr(true);
    CGM.getSanitizerMetadata()->disableSanitizerForGlobal(InfoPtr);
    Args.push_back(Builder.CreateBitCast(InfoPtr, Int8PtrTy));
    ArgTypes.push_back(Int8PtrTy);
  }

  for (size_t i = 0, n = DynamicArgs.size(); i != n; ++i) {
    Args.push_back(EmitCheckValue(DynamicArgs[i]));
    ArgTypes.push_back(IntPtrTy);
  }

  llvm::FunctionType *FnType =
    llvm::FunctionType::get(CGM.VoidTy, ArgTypes, false);

  if (!FatalCond || !RecoverableCond) {
    // Simple case: we need to generate a single handler call, either
    // fatal, or non-fatal.
    emitCheckHandlerCall(*this, FnType, Args, CheckName, RecoverKind,
                         (FatalCond != nullptr), Cont);
  } else {
    // Emit two handler calls: first one for set of unrecoverable checks,
    // another one for recoverable.
    llvm::BasicBlock *NonFatalHandlerBB =
        createBasicBlock("non_fatal." + CheckName);
    llvm::BasicBlock *FatalHandlerBB = createBasicBlock("fatal." + CheckName);
    Builder.CreateCondBr(FatalCond, NonFatalHandlerBB, FatalHandlerBB);
    EmitBlock(FatalHandlerBB);
    emitCheckHandlerCall(*this, FnType, Args, CheckName, RecoverKind, true,
                         NonFatalHandlerBB);
    EmitBlock(NonFatalHandlerBB);
    emitCheckHandlerCall(*this, FnType, Args, CheckName, RecoverKind, false,
                         Cont);
  }

  EmitBlock(Cont);
}

/**
Paul: generic function for emiting an object tracing call.*/
void CodeGenFunction::HexEmitObjTraceInst(
  StringRef InstName, ArrayRef<llvm::Value *> DynamicArgs) {
  SmallVector<llvm::Value *, 4> Args;
  SmallVector<llvm::Type *, 4> ArgTypes;
  Args.reserve(DynamicArgs.size() + 1);
  ArgTypes.reserve(DynamicArgs.size() + 1);

  for (size_t i = 0, n = DynamicArgs.size(); i != n; ++i) {
    Args.push_back(EmitCheckValue(DynamicArgs[i]));
    ArgTypes.push_back(IntPtrTy);
  }

  llvm::AttrBuilder B;
  B.addAttribute(llvm::Attribute::UWTable);

  llvm::FunctionType *FnType =
    llvm::FunctionType::get(CGM.VoidTy, ArgTypes, false);

  llvm::Value *Fn = CGM.CreateRuntimeFunction(
    FnType, InstName,
    llvm::AttributeSet::get(getLLVMContext(),
                            llvm::AttributeSet::FunctionIndex, B));
  if(Fn != NULL && FnType != NULL) {
    EmitNounwindRuntimeCall(Fn, Args);
  }
}

void CodeGenFunction::EmitCfiSlowPathCheck(
    SanitizerMask Kind, llvm::Value *Cond, llvm::ConstantInt *TypeId,
    llvm::Value *Ptr, ArrayRef<llvm::Constant *> StaticArgs) {
  llvm::BasicBlock *Cont = createBasicBlock("cfi.cont");

  llvm::BasicBlock *CheckBB = createBasicBlock("cfi.slowpath");
  llvm::BranchInst *BI = Builder.CreateCondBr(Cond, Cont, CheckBB);

  llvm::MDBuilder MDHelper(getLLVMContext());
  llvm::MDNode *Node = MDHelper.createBranchWeights((1U << 20) - 1, 1);
  BI->setMetadata(llvm::LLVMContext::MD_prof, Node);

  EmitBlock(CheckBB);

  bool WithDiag = !CGM.getCodeGenOpts().SanitizeTrap.has(Kind);

  llvm::CallInst *CheckCall;
  if (WithDiag) {
    llvm::Constant *Info = llvm::ConstantStruct::getAnon(StaticArgs);
    auto *InfoPtr =
        new llvm::GlobalVariable(CGM.getModule(), Info->getType(), false,
                                 llvm::GlobalVariable::PrivateLinkage, Info);
    InfoPtr->setUnnamedAddr(true);
    CGM.getSanitizerMetadata()->disableSanitizerForGlobal(InfoPtr);

    llvm::Constant *SlowPathDiagFn = CGM.getModule().getOrInsertFunction(
        "__cfi_slowpath_diag",
        llvm::FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy, Int8PtrTy},
                                false));
    CheckCall = Builder.CreateCall(
        SlowPathDiagFn,
        {TypeId, Ptr, Builder.CreateBitCast(InfoPtr, Int8PtrTy)});
  } else {
    llvm::Constant *SlowPathFn = CGM.getModule().getOrInsertFunction(
        "__cfi_slowpath",
        llvm::FunctionType::get(VoidTy, {Int64Ty, Int8PtrTy}, false));
    CheckCall = Builder.CreateCall(SlowPathFn, {TypeId, Ptr});
  }

  CheckCall->setDoesNotThrow();

  EmitBlock(Cont);
}

// This function is basically a switch over the CFI failure kind, which is
// extracted from CFICheckFailData (1st function argument). Each case is either
// llvm.trap or a call to one of the two runtime handlers, based on
// -fsanitize-trap and -fsanitize-recover settings.  Default case (invalid
// failure kind) traps, but this should really never happen.  CFICheckFailData
// can be nullptr if the calling module has -fsanitize-trap behavior for this
// check kind; in this case __cfi_check_fail traps as well.
void CodeGenFunction::EmitCfiCheckFail() {
  SanitizerScope SanScope(this);
  FunctionArgList Args;
  ImplicitParamDecl ArgData(getContext(), nullptr, SourceLocation(), nullptr,
                            getContext().VoidPtrTy);
  ImplicitParamDecl ArgAddr(getContext(), nullptr, SourceLocation(), nullptr,
                            getContext().VoidPtrTy);
  Args.push_back(&ArgData);
  Args.push_back(&ArgAddr);

  const CGFunctionInfo &FI =
    CGM.getTypes().arrangeBuiltinFunctionDeclaration(getContext().VoidTy, Args);

  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {VoidPtrTy, VoidPtrTy}, false),
      llvm::GlobalValue::WeakODRLinkage, "__cfi_check_fail", &CGM.getModule());
  F->setVisibility(llvm::GlobalValue::HiddenVisibility);

  StartFunction(GlobalDecl(), CGM.getContext().VoidTy, F, FI, Args,
                SourceLocation());

  llvm::Value *Data =
      EmitLoadOfScalar(GetAddrOfLocalVar(&ArgData), /*Volatile=*/false,
                       CGM.getContext().VoidPtrTy, ArgData.getLocation());
  llvm::Value *Addr =
      EmitLoadOfScalar(GetAddrOfLocalVar(&ArgAddr), /*Volatile=*/false,
                       CGM.getContext().VoidPtrTy, ArgAddr.getLocation());

  // Data == nullptr means the calling module has trap behaviour for this check.
  llvm::Value *DataIsNotNullPtr =
      Builder.CreateICmpNE(Data, llvm::ConstantPointerNull::get(Int8PtrTy));
  EmitTrapCheck(DataIsNotNullPtr);

  llvm::StructType *SourceLocationTy =
      llvm::StructType::get(VoidPtrTy, Int32Ty, Int32Ty, nullptr);
  llvm::StructType *CfiCheckFailDataTy =
      llvm::StructType::get(Int8Ty, SourceLocationTy, VoidPtrTy, nullptr);

  llvm::Value *V = Builder.CreateConstGEP2_32(
      CfiCheckFailDataTy,
      Builder.CreatePointerCast(Data, CfiCheckFailDataTy->getPointerTo(0)), 0,
      0);
  Address CheckKindAddr(V, getIntAlign());
  llvm::Value *CheckKind = Builder.CreateLoad(CheckKindAddr);

  llvm::Value *AllVtables = llvm::MetadataAsValue::get(
      CGM.getLLVMContext(),
      llvm::MDString::get(CGM.getLLVMContext(), "all-vtables"));
  llvm::Value *ValidVtable = Builder.CreateZExt(
      Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::bitset_test),
                         {Addr, AllVtables}),
      IntPtrTy);

  const std::pair<int, SanitizerMask> CheckKinds[] = {
      {CFITCK_VCall, SanitizerKind::CFIVCall},
      {CFITCK_NVCall, SanitizerKind::CFINVCall},
      {CFITCK_DerivedCast, SanitizerKind::CFIDerivedCast},
      {CFITCK_UnrelatedCast, SanitizerKind::CFIUnrelatedCast},
      {CFITCK_ICall, SanitizerKind::CFIICall}};

  SmallVector<std::pair<llvm::Value *, SanitizerMask>, 5> Checks;
  for (auto CheckKindMaskPair : CheckKinds) {
    int Kind = CheckKindMaskPair.first;
    SanitizerMask Mask = CheckKindMaskPair.second;
    llvm::Value *Cond =
        Builder.CreateICmpNE(CheckKind, llvm::ConstantInt::get(Int8Ty, Kind));
    EmitCheck(std::make_pair(Cond, Mask), "cfi_check_fail", {},
              {Data, Addr, ValidVtable});
  }

  FinishFunction();
  // The only reference to this function will be created during LTO link.
  // Make sure it survives until then.
  CGM.addUsedGlobal(F);
}

void CodeGenFunction::EmitTrapCheck(llvm::Value *Checked) {
  llvm::BasicBlock *Cont = createBasicBlock("cont");

  // If we're optimizing, collapse all calls to trap down to just one per
  // function to save on code size.
  if (!CGM.getCodeGenOpts().OptimizationLevel || !TrapBB) {
    TrapBB = createBasicBlock("trap");
    Builder.CreateCondBr(Checked, Cont, TrapBB);
    EmitBlock(TrapBB);
    llvm::CallInst *TrapCall = EmitTrapCall(llvm::Intrinsic::trap);
    TrapCall->setDoesNotReturn();
    TrapCall->setDoesNotThrow();
    Builder.CreateUnreachable();
  } else {
    Builder.CreateCondBr(Checked, Cont, TrapBB);
  }

  EmitBlock(Cont);
}

llvm::CallInst *CodeGenFunction::EmitTrapCall(llvm::Intrinsic::ID IntrID) {
  llvm::CallInst *TrapCall = Builder.CreateCall(CGM.getIntrinsic(IntrID));

  if (!CGM.getCodeGenOpts().TrapFuncName.empty())
    TrapCall->addAttribute(llvm::AttributeSet::FunctionIndex,
                           "trap-func-name",
                           CGM.getCodeGenOpts().TrapFuncName);

  return TrapCall;
}

Address CodeGenFunction::EmitArrayToPointerDecay(const Expr *E,
                                                 AlignmentSource *AlignSource) {
  assert(E->getType()->isArrayType() &&
         "Array to pointer decay must have array source type!");

  // Expressions of array type can't be bitfields or vector elements.
  LValue LV = EmitLValue(E);
  Address Addr = LV.getAddress();
  if (AlignSource) *AlignSource = LV.getAlignmentSource();

  // If the array type was an incomplete type, we need to make sure
  // the decay ends up being the right type.
  llvm::Type *NewTy = ConvertType(E->getType());
  Addr = Builder.CreateElementBitCast(Addr, NewTy);

  // Note that VLA pointers are always decayed, so we don't need to do
  // anything here.
  if (!E->getType()->isVariableArrayType()) {
    assert(isa<llvm::ArrayType>(Addr.getElementType()) &&
           "Expected pointer to array");
    Addr = Builder.CreateStructGEP(Addr, 0, CharUnits::Zero(), "arraydecay");
  }

  QualType EltType = E->getType()->castAsArrayTypeUnsafe()->getElementType();
  return Builder.CreateElementBitCast(Addr, ConvertTypeForMem(EltType));
}

/// isSimpleArrayDecayOperand - If the specified expr is a simple decay from an
/// array to pointer, return the array subexpression.
static const Expr *isSimpleArrayDecayOperand(const Expr *E) {
  // If this isn't just an array->pointer decay, bail out.
  const auto *CE = dyn_cast<CastExpr>(E);
  if (!CE || CE->getCastKind() != CK_ArrayToPointerDecay)
    return nullptr;

  // If this is a decay from variable width array, bail out.
  const Expr *SubExpr = CE->getSubExpr();
  if (SubExpr->getType()->isVariableArrayType())
    return nullptr;

  return SubExpr;
}

static llvm::Value *emitArraySubscriptGEP(CodeGenFunction &CGF,
                                          llvm::Value *ptr,
                                          ArrayRef<llvm::Value*> indices,
                                          bool inbounds,
                                    const llvm::Twine &name = "arrayidx") {
  if (inbounds) {
    return CGF.Builder.CreateInBoundsGEP(ptr, indices, name);
  } else {
    return CGF.Builder.CreateGEP(ptr, indices, name);
  }
}

static CharUnits getArrayElementAlign(CharUnits arrayAlign,
                                      llvm::Value *idx,
                                      CharUnits eltSize) {
  // If we have a constant index, we can use the exact offset of the
  // element we're accessing.
  if (auto constantIdx = dyn_cast<llvm::ConstantInt>(idx)) {
    CharUnits offset = constantIdx->getZExtValue() * eltSize;
    return arrayAlign.alignmentAtOffset(offset);

  // Otherwise, use the worst-case alignment for any element.
  } else {
    return arrayAlign.alignmentOfArrayElement(eltSize);
  }
}

static QualType getFixedSizeElementType(const ASTContext &ctx,
                                        const VariableArrayType *vla) {
  QualType eltType;
  do {
    eltType = vla->getElementType();
  } while ((vla = ctx.getAsVariableArrayType(eltType)));
  return eltType;
}

static Address emitArraySubscriptGEP(CodeGenFunction &CGF, Address addr,
                                     ArrayRef<llvm::Value*> indices,
                                     QualType eltType, bool inbounds,
                                     const llvm::Twine &name = "arrayidx") {
  // All the indices except that last must be zero.
#ifndef NDEBUG
  for (auto idx : indices.drop_back())
    assert(isa<llvm::ConstantInt>(idx) &&
           cast<llvm::ConstantInt>(idx)->isZero());
#endif  

  // Determine the element size of the statically-sized base.  This is
  // the thing that the indices are expressed in terms of.
  if (auto vla = CGF.getContext().getAsVariableArrayType(eltType)) {
    eltType = getFixedSizeElementType(CGF.getContext(), vla);
  }

  // We can use that to compute the best alignment of the element.
  CharUnits eltSize = CGF.getContext().getTypeSizeInChars(eltType);
  CharUnits eltAlign =
    getArrayElementAlign(addr.getAlignment(), indices.back(), eltSize);

  llvm::Value *eltPtr =
    emitArraySubscriptGEP(CGF, addr.getPointer(), indices, inbounds, name);
  return Address(eltPtr, eltAlign);
}

LValue CodeGenFunction::EmitArraySubscriptExpr(const ArraySubscriptExpr *E,
                                               bool Accessed) {
  // The index must always be an integer, which is not an aggregate.  Emit it.
  llvm::Value *Idx = EmitScalarExpr(E->getIdx());
  QualType IdxTy  = E->getIdx()->getType();
  bool IdxSigned = IdxTy->isSignedIntegerOrEnumerationType();

  if (SanOpts.has(SanitizerKind::ArrayBounds))
    EmitBoundsCheck(E, E->getBase(), Idx, IdxTy, Accessed);

  // If the base is a vector type, then we are forming a vector element lvalue
  // with this subscript.
  if (E->getBase()->getType()->isVectorType() &&
      !isa<ExtVectorElementExpr>(E->getBase())) {
    // Emit the vector as an lvalue to get its address.
    LValue LHS = EmitLValue(E->getBase());
    assert(LHS.isSimple() && "Can only subscript lvalue vectors here!");
    return LValue::MakeVectorElt(LHS.getAddress(), Idx,
                                 E->getBase()->getType(),
                                 LHS.getAlignmentSource());
  }

  // All the other cases basically behave like simple offsetting.

  // Extend or truncate the index type to 32 or 64-bits.
  if (Idx->getType() != IntPtrTy)
    Idx = Builder.CreateIntCast(Idx, IntPtrTy, IdxSigned, "idxprom");

  // Handle the extvector case we ignored above.
  if (isa<ExtVectorElementExpr>(E->getBase())) {
    LValue LV = EmitLValue(E->getBase());
    Address Addr = EmitExtVectorElementLValue(LV);

    QualType EltType = LV.getType()->castAs<VectorType>()->getElementType();
    Addr = emitArraySubscriptGEP(*this, Addr, Idx, EltType, /*inbounds*/ true);
    return MakeAddrLValue(Addr, EltType, LV.getAlignmentSource());
  }

  AlignmentSource AlignSource;
  Address Addr = Address::invalid();
  if (const VariableArrayType *vla =
           getContext().getAsVariableArrayType(E->getType())) {
    // The base must be a pointer, which is not an aggregate.  Emit
    // it.  It needs to be emitted first in case it's what captures
    // the VLA bounds.
    Addr = EmitPointerWithAlignment(E->getBase(), &AlignSource);

    // The element count here is the total number of non-VLA elements.
    llvm::Value *numElements = getVLASize(vla).first;

    // Effectively, the multiply by the VLA size is part of the GEP.
    // GEP indexes are signed, and scaling an index isn't permitted to
    // signed-overflow, so we use the same semantics for our explicit
    // multiply.  We suppress this if overflow is not undefined behavior.
    if (getLangOpts().isSignedOverflowDefined()) {
      Idx = Builder.CreateMul(Idx, numElements);
    } else {
      Idx = Builder.CreateNSWMul(Idx, numElements);
    }

    Addr = emitArraySubscriptGEP(*this, Addr, Idx, vla->getElementType(),
                                 !getLangOpts().isSignedOverflowDefined());

  } else if (const ObjCObjectType *OIT = E->getType()->getAs<ObjCObjectType>()){
    // Indexing over an interface, as in "NSString *P; P[4];"
    CharUnits InterfaceSize = getContext().getTypeSizeInChars(OIT);
    llvm::Value *InterfaceSizeVal = 
      llvm::ConstantInt::get(Idx->getType(), InterfaceSize.getQuantity());;

    llvm::Value *ScaledIdx = Builder.CreateMul(Idx, InterfaceSizeVal);

    // Emit the base pointer.
    Addr = EmitPointerWithAlignment(E->getBase(), &AlignSource);

    // We don't necessarily build correct LLVM struct types for ObjC
    // interfaces, so we can't rely on GEP to do this scaling
    // correctly, so we need to cast to i8*.  FIXME: is this actually
    // true?  A lot of other things in the fragile ABI would break...
    llvm::Type *OrigBaseTy = Addr.getType();
    Addr = Builder.CreateElementBitCast(Addr, Int8Ty);

    // Do the GEP.
    CharUnits EltAlign =
      getArrayElementAlign(Addr.getAlignment(), Idx, InterfaceSize);
    llvm::Value *EltPtr =
      emitArraySubscriptGEP(*this, Addr.getPointer(), ScaledIdx, false);
    Addr = Address(EltPtr, EltAlign);

    // Cast back.
    Addr = Builder.CreateBitCast(Addr, OrigBaseTy);
  } else if (const Expr *Array = isSimpleArrayDecayOperand(E->getBase())) {
    // If this is A[i] where A is an array, the frontend will have decayed the
    // base to be a ArrayToPointerDecay implicit cast.  While correct, it is
    // inefficient at -O0 to emit a "gep A, 0, 0" when codegen'ing it, then a
    // "gep x, i" here.  Emit one "gep A, 0, i".
    assert(Array->getType()->isArrayType() &&
           "Array to pointer decay must have array source type!");
    LValue ArrayLV;
    // For simple multidimensional array indexing, set the 'accessed' flag for
    // better bounds-checking of the base expression.
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Array))
      ArrayLV = EmitArraySubscriptExpr(ASE, /*Accessed*/ true);
    else
      ArrayLV = EmitLValue(Array);

    // Propagate the alignment from the array itself to the result.
    Addr = emitArraySubscriptGEP(*this, ArrayLV.getAddress(),
                                 {CGM.getSize(CharUnits::Zero()), Idx},
                                 E->getType(),
                                 !getLangOpts().isSignedOverflowDefined());
    AlignSource = ArrayLV.getAlignmentSource();
  } else {
    // The base must be a pointer; emit it with an estimate of its alignment.
    Addr = EmitPointerWithAlignment(E->getBase(), &AlignSource);
    Addr = emitArraySubscriptGEP(*this, Addr, Idx, E->getType(),
                                 !getLangOpts().isSignedOverflowDefined());
  }

  LValue LV = MakeAddrLValue(Addr, E->getType(), AlignSource);

  // TODO: Preserve/extend path TBAA metadata?

  if (getLangOpts().ObjC1 &&
      getLangOpts().getGC() != LangOptions::NonGC) {
    LV.setNonGC(!E->isOBJCGCCandidate(getContext()));
    setObjCGCLValueClass(getContext(), E, LV);
  }
  return LV;
}

static Address emitOMPArraySectionBase(CodeGenFunction &CGF, const Expr *Base,
                                       AlignmentSource &AlignSource,
                                       QualType BaseTy, QualType ElTy,
                                       bool IsLowerBound) {
  LValue BaseLVal;
  if (auto *ASE = dyn_cast<OMPArraySectionExpr>(Base->IgnoreParenImpCasts())) {
    BaseLVal = CGF.EmitOMPArraySectionExpr(ASE, IsLowerBound);
    if (BaseTy->isArrayType()) {
      Address Addr = BaseLVal.getAddress();
      AlignSource = BaseLVal.getAlignmentSource();

      // If the array type was an incomplete type, we need to make sure
      // the decay ends up being the right type.
      llvm::Type *NewTy = CGF.ConvertType(BaseTy);
      Addr = CGF.Builder.CreateElementBitCast(Addr, NewTy);

      // Note that VLA pointers are always decayed, so we don't need to do
      // anything here.
      if (!BaseTy->isVariableArrayType()) {
        assert(isa<llvm::ArrayType>(Addr.getElementType()) &&
               "Expected pointer to array");
        Addr = CGF.Builder.CreateStructGEP(Addr, 0, CharUnits::Zero(),
                                           "arraydecay");
      }

      return CGF.Builder.CreateElementBitCast(Addr,
                                              CGF.ConvertTypeForMem(ElTy));
    }
    CharUnits Align = CGF.getNaturalTypeAlignment(ElTy, &AlignSource);
    return Address(CGF.Builder.CreateLoad(BaseLVal.getAddress()), Align);
  }
  return CGF.EmitPointerWithAlignment(Base, &AlignSource);
}

LValue CodeGenFunction::EmitOMPArraySectionExpr(const OMPArraySectionExpr *E,
                                                bool IsLowerBound) {
  QualType BaseTy;
  if (auto *ASE =
          dyn_cast<OMPArraySectionExpr>(E->getBase()->IgnoreParenImpCasts()))
    BaseTy = OMPArraySectionExpr::getBaseOriginalType(ASE);
  else
    BaseTy = E->getBase()->getType();
  QualType ResultExprTy;
  if (auto *AT = getContext().getAsArrayType(BaseTy))
    ResultExprTy = AT->getElementType();
  else
    ResultExprTy = BaseTy->getPointeeType();
  llvm::Value *Idx = nullptr;
  if (IsLowerBound || (!IsLowerBound && E->getColonLoc().isInvalid())) {
    // Requesting lower bound or upper bound, but without provided length and
    // without ':' symbol for the default length -> length = 1.
    // Idx = LowerBound ?: 0;
    if (auto *LowerBound = E->getLowerBound()) {
      Idx = Builder.CreateIntCast(
          EmitScalarExpr(LowerBound), IntPtrTy,
          LowerBound->getType()->hasSignedIntegerRepresentation());
    } else
      Idx = llvm::ConstantInt::getNullValue(IntPtrTy);
  } else {
    // Try to emit length or lower bound as constant. If this is possible, 1
    // is subtracted from constant length or lower bound. Otherwise, emit LLVM
    // IR (LB + Len) - 1.
    auto &C = CGM.getContext();
    auto *Length = E->getLength();
    llvm::APSInt ConstLength;
    if (Length) {
      // Idx = LowerBound + Length - 1;
      if (Length->isIntegerConstantExpr(ConstLength, C)) {
        ConstLength = ConstLength.zextOrTrunc(PointerWidthInBits);
        Length = nullptr;
      }
      auto *LowerBound = E->getLowerBound();
      llvm::APSInt ConstLowerBound(PointerWidthInBits, /*isUnsigned=*/false);
      if (LowerBound && LowerBound->isIntegerConstantExpr(ConstLowerBound, C)) {
        ConstLowerBound = ConstLowerBound.zextOrTrunc(PointerWidthInBits);
        LowerBound = nullptr;
      }
      if (!Length)
        --ConstLength;
      else if (!LowerBound)
        --ConstLowerBound;

      if (Length || LowerBound) {
        auto *LowerBoundVal =
            LowerBound
                ? Builder.CreateIntCast(
                      EmitScalarExpr(LowerBound), IntPtrTy,
                      LowerBound->getType()->hasSignedIntegerRepresentation())
                : llvm::ConstantInt::get(IntPtrTy, ConstLowerBound);
        auto *LengthVal =
            Length
                ? Builder.CreateIntCast(
                      EmitScalarExpr(Length), IntPtrTy,
                      Length->getType()->hasSignedIntegerRepresentation())
                : llvm::ConstantInt::get(IntPtrTy, ConstLength);
        Idx = Builder.CreateAdd(LowerBoundVal, LengthVal, "lb_add_len",
                                /*HasNUW=*/false,
                                !getLangOpts().isSignedOverflowDefined());
        if (Length && LowerBound) {
          Idx = Builder.CreateSub(
              Idx, llvm::ConstantInt::get(IntPtrTy, /*V=*/1), "idx_sub_1",
              /*HasNUW=*/false, !getLangOpts().isSignedOverflowDefined());
        }
      } else
        Idx = llvm::ConstantInt::get(IntPtrTy, ConstLength + ConstLowerBound);
    } else {
      // Idx = ArraySize - 1;
      QualType ArrayTy = BaseTy->isPointerType()
                             ? E->getBase()->IgnoreParenImpCasts()->getType()
                             : BaseTy;
      if (auto *VAT = C.getAsVariableArrayType(ArrayTy)) {
        Length = VAT->getSizeExpr();
        if (Length->isIntegerConstantExpr(ConstLength, C))
          Length = nullptr;
      } else {
        auto *CAT = C.getAsConstantArrayType(ArrayTy);
        ConstLength = CAT->getSize();
      }
      if (Length) {
        auto *LengthVal = Builder.CreateIntCast(
            EmitScalarExpr(Length), IntPtrTy,
            Length->getType()->hasSignedIntegerRepresentation());
        Idx = Builder.CreateSub(
            LengthVal, llvm::ConstantInt::get(IntPtrTy, /*V=*/1), "len_sub_1",
            /*HasNUW=*/false, !getLangOpts().isSignedOverflowDefined());
      } else {
        ConstLength = ConstLength.zextOrTrunc(PointerWidthInBits);
        --ConstLength;
        Idx = llvm::ConstantInt::get(IntPtrTy, ConstLength);
      }
    }
  }
  assert(Idx);

  Address EltPtr = Address::invalid();
  AlignmentSource AlignSource;
  if (auto *VLA = getContext().getAsVariableArrayType(ResultExprTy)) {
    // The base must be a pointer, which is not an aggregate.  Emit
    // it.  It needs to be emitted first in case it's what captures
    // the VLA bounds.
    Address Base =
        emitOMPArraySectionBase(*this, E->getBase(), AlignSource, BaseTy,
                                VLA->getElementType(), IsLowerBound);
    // The element count here is the total number of non-VLA elements.
    llvm::Value *NumElements = getVLASize(VLA).first;

    // Effectively, the multiply by the VLA size is part of the GEP.
    // GEP indexes are signed, and scaling an index isn't permitted to
    // signed-overflow, so we use the same semantics for our explicit
    // multiply.  We suppress this if overflow is not undefined behavior.
    if (getLangOpts().isSignedOverflowDefined())
      Idx = Builder.CreateMul(Idx, NumElements);
    else
      Idx = Builder.CreateNSWMul(Idx, NumElements);
    EltPtr = emitArraySubscriptGEP(*this, Base, Idx, VLA->getElementType(),
                                   !getLangOpts().isSignedOverflowDefined());
  } else if (const Expr *Array = isSimpleArrayDecayOperand(E->getBase())) {
    // If this is A[i] where A is an array, the frontend will have decayed the
    // base to be a ArrayToPointerDecay implicit cast.  While correct, it is
    // inefficient at -O0 to emit a "gep A, 0, 0" when codegen'ing it, then a
    // "gep x, i" here.  Emit one "gep A, 0, i".
    assert(Array->getType()->isArrayType() &&
           "Array to pointer decay must have array source type!");
    LValue ArrayLV;
    // For simple multidimensional array indexing, set the 'accessed' flag for
    // better bounds-checking of the base expression.
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Array))
      ArrayLV = EmitArraySubscriptExpr(ASE, /*Accessed*/ true);
    else
      ArrayLV = EmitLValue(Array);

    // Propagate the alignment from the array itself to the result.
    EltPtr = emitArraySubscriptGEP(
        *this, ArrayLV.getAddress(), {CGM.getSize(CharUnits::Zero()), Idx},
        ResultExprTy, !getLangOpts().isSignedOverflowDefined());
    AlignSource = ArrayLV.getAlignmentSource();
  } else {
    Address Base = emitOMPArraySectionBase(*this, E->getBase(), AlignSource,
                                           BaseTy, ResultExprTy, IsLowerBound);
    EltPtr = emitArraySubscriptGEP(*this, Base, Idx, ResultExprTy,
                                   !getLangOpts().isSignedOverflowDefined());
  }

  return MakeAddrLValue(EltPtr, ResultExprTy, AlignSource);
}

LValue CodeGenFunction::
EmitExtVectorElementExpr(const ExtVectorElementExpr *E) {
  // Emit the base vector as an l-value.
  LValue Base;

  // ExtVectorElementExpr's base can either be a vector or pointer to vector.
  if (E->isArrow()) {
    // If it is a pointer to a vector, emit the address and form an lvalue with
    // it.
    AlignmentSource AlignSource;
    Address Ptr = EmitPointerWithAlignment(E->getBase(), &AlignSource);
    const PointerType *PT = E->getBase()->getType()->getAs<PointerType>();
    Base = MakeAddrLValue(Ptr, PT->getPointeeType(), AlignSource);
    Base.getQuals().removeObjCGCAttr();
  } else if (E->getBase()->isGLValue()) {
    // Otherwise, if the base is an lvalue ( as in the case of foo.x.x),
    // emit the base as an lvalue.
    assert(E->getBase()->getType()->isVectorType());
    Base = EmitLValue(E->getBase());
  } else {
    // Otherwise, the base is a normal rvalue (as in (V+V).x), emit it as such.
    assert(E->getBase()->getType()->isVectorType() &&
           "Result must be a vector");
    llvm::Value *Vec = EmitScalarExpr(E->getBase());

    // Store the vector to memory (because LValue wants an address).
    Address VecMem = CreateMemTemp(E->getBase()->getType());
    Builder.CreateStore(Vec, VecMem);
    Base = MakeAddrLValue(VecMem, E->getBase()->getType(),
                          AlignmentSource::Decl);
  }

  QualType type =
    E->getType().withCVRQualifiers(Base.getQuals().getCVRQualifiers());

  // Encode the element access list into a vector of unsigned indices.
  SmallVector<uint32_t, 4> Indices;
  E->getEncodedElementAccess(Indices);

  if (Base.isSimple()) {
    llvm::Constant *CV =
        llvm::ConstantDataVector::get(getLLVMContext(), Indices);
    return LValue::MakeExtVectorElt(Base.getAddress(), CV, type,
                                    Base.getAlignmentSource());
  }
  assert(Base.isExtVectorElt() && "Can only subscript lvalue vec elts here!");

  llvm::Constant *BaseElts = Base.getExtVectorElts();
  SmallVector<llvm::Constant *, 4> CElts;

  for (unsigned i = 0, e = Indices.size(); i != e; ++i)
    CElts.push_back(BaseElts->getAggregateElement(Indices[i]));
  llvm::Constant *CV = llvm::ConstantVector::get(CElts);
  return LValue::MakeExtVectorElt(Base.getExtVectorAddress(), CV, type,
                                  Base.getAlignmentSource());
}

LValue CodeGenFunction::EmitMemberExpr(const MemberExpr *E) {
  Expr *BaseExpr = E->getBase();

  // If this is s.x, emit s as an lvalue.  If it is s->x, emit s as a scalar.
  LValue BaseLV;
  if (E->isArrow()) {
    AlignmentSource AlignSource;
    Address Addr = EmitPointerWithAlignment(BaseExpr, &AlignSource);
    QualType PtrTy = BaseExpr->getType()->getPointeeType();
    EmitTypeCheck(TCK_MemberAccess, E->getExprLoc(), Addr.getPointer(), PtrTy);
    BaseLV = MakeAddrLValue(Addr, PtrTy, AlignSource);
  } else
    BaseLV = EmitCheckedLValue(BaseExpr, TCK_MemberAccess);

  NamedDecl *ND = E->getMemberDecl();
  if (auto *Field = dyn_cast<FieldDecl>(ND)) {
    LValue LV = EmitLValueForField(BaseLV, Field);
    setObjCGCLValueClass(getContext(), E, LV);
    return LV;
  }

  if (auto *VD = dyn_cast<VarDecl>(ND))
    return EmitGlobalVarDeclLValue(*this, E, VD);

  if (const auto *FD = dyn_cast<FunctionDecl>(ND))
    return EmitFunctionDeclLValue(*this, E, FD);

  llvm_unreachable("Unhandled member declaration!");
}

/// Given that we are currently emitting a lambda, emit an l-value for
/// one of its members.
LValue CodeGenFunction::EmitLValueForLambdaField(const FieldDecl *Field) {
  assert(cast<CXXMethodDecl>(CurCodeDecl)->getParent()->isLambda());
  assert(cast<CXXMethodDecl>(CurCodeDecl)->getParent() == Field->getParent());
  QualType LambdaTagType =
    getContext().getTagDeclType(Field->getParent());
  LValue LambdaLV = MakeNaturalAlignAddrLValue(CXXABIThisValue, LambdaTagType);
  return EmitLValueForField(LambdaLV, Field);
}

/// Drill down to the storage of a field without walking into
/// reference types.
///
/// The resulting address doesn't necessarily have the right type.
static Address emitAddrOfFieldStorage(CodeGenFunction &CGF, Address base,
                                      const FieldDecl *field) {
  const RecordDecl *rec = field->getParent();
  
  unsigned idx =
    CGF.CGM.getTypes().getCGRecordLayout(rec).getLLVMFieldNo(field);

  CharUnits offset;
  // Adjust the alignment down to the given offset.
  // As a special case, if the LLVM field index is 0, we know that this
  // is zero.
  assert((idx != 0 || CGF.getContext().getASTRecordLayout(rec)
                         .getFieldOffset(field->getFieldIndex()) == 0) &&
         "LLVM field at index zero had non-zero offset?");
  if (idx != 0) {
    auto &recLayout = CGF.getContext().getASTRecordLayout(rec);
    auto offsetInBits = recLayout.getFieldOffset(field->getFieldIndex());
    offset = CGF.getContext().toCharUnitsFromBits(offsetInBits);
  }

  return CGF.Builder.CreateStructGEP(base, idx, offset, field->getName());
}

LValue CodeGenFunction::EmitLValueForField(LValue base,
                                           const FieldDecl *field) {
  AlignmentSource fieldAlignSource =
    getFieldAlignmentSource(base.getAlignmentSource());

  if (field->isBitField()) {
    const CGRecordLayout &RL =
      CGM.getTypes().getCGRecordLayout(field->getParent());
    const CGBitFieldInfo &Info = RL.getBitFieldInfo(field);
    Address Addr = base.getAddress();
    unsigned Idx = RL.getLLVMFieldNo(field);
    if (Idx != 0)
      // For structs, we GEP to the field that the record layout suggests.
      Addr = Builder.CreateStructGEP(Addr, Idx, Info.StorageOffset,
                                     field->getName());
    // Get the access type.
    llvm::Type *FieldIntTy =
      llvm::Type::getIntNTy(getLLVMContext(), Info.StorageSize);
    if (Addr.getElementType() != FieldIntTy)
      Addr = Builder.CreateElementBitCast(Addr, FieldIntTy);

    QualType fieldType =
      field->getType().withCVRQualifiers(base.getVRQualifiers());
    return LValue::MakeBitfield(Addr, Info, fieldType, fieldAlignSource);
  }

  const RecordDecl *rec = field->getParent();
  QualType type = field->getType();

  bool mayAlias = rec->hasAttr<MayAliasAttr>();

  Address addr = base.getAddress();
  unsigned cvr = base.getVRQualifiers();
  bool TBAAPath = CGM.getCodeGenOpts().StructPathTBAA;
  if (rec->isUnion()) {
    // For unions, there is no pointer adjustment.
    assert(!type->isReferenceType() && "union has reference member");
    // TODO: handle path-aware TBAA for union.
    TBAAPath = false;
  } else {
    // For structs, we GEP to the field that the record layout suggests.
    addr = emitAddrOfFieldStorage(*this, addr, field);

    // If this is a reference field, load the reference right now.
    if (const ReferenceType *refType = type->getAs<ReferenceType>()) {
      llvm::LoadInst *load = Builder.CreateLoad(addr, "ref");
      if (cvr & Qualifiers::Volatile) load->setVolatile(true);

      // Loading the reference will disable path-aware TBAA.
      TBAAPath = false;
      if (CGM.shouldUseTBAA()) {
        llvm::MDNode *tbaa;
        if (mayAlias)
          tbaa = CGM.getTBAAInfo(getContext().CharTy);
        else
          tbaa = CGM.getTBAAInfo(type);
        if (tbaa)
          CGM.DecorateInstructionWithTBAA(load, tbaa);
      }

      mayAlias = false;
      type = refType->getPointeeType();

      CharUnits alignment =
        getNaturalTypeAlignment(type, &fieldAlignSource, /*pointee*/ true);
      addr = Address(load, alignment);

      // Qualifiers on the struct don't apply to the referencee, and
      // we'll pick up CVR from the actual type later, so reset these
      // additional qualifiers now.
      cvr = 0;
    }
  }

  // Make sure that the address is pointing to the right type.  This is critical
  // for both unions and structs.  A union needs a bitcast, a struct element
  // will need a bitcast if the LLVM type laid out doesn't match the desired
  // type.
  addr = Builder.CreateElementBitCast(addr,
                                      CGM.getTypes().ConvertTypeForMem(type),
                                      field->getName());

  if (field->hasAttr<AnnotateAttr>())
    addr = EmitFieldAnnotations(field, addr);

  LValue LV = MakeAddrLValue(addr, type, fieldAlignSource);
  LV.getQuals().addCVRQualifiers(cvr);
  if (TBAAPath) {
    const ASTRecordLayout &Layout =
        getContext().getASTRecordLayout(field->getParent());
    // Set the base type to be the base type of the base LValue and
    // update offset to be relative to the base type.
    LV.setTBAABaseType(mayAlias ? getContext().CharTy : base.getTBAABaseType());
    LV.setTBAAOffset(mayAlias ? 0 : base.getTBAAOffset() +
                     Layout.getFieldOffset(field->getFieldIndex()) /
                                           getContext().getCharWidth());
  }

  // __weak attribute on a field is ignored.
  if (LV.getQuals().getObjCGCAttr() == Qualifiers::Weak)
    LV.getQuals().removeObjCGCAttr();

  // Fields of may_alias structs act like 'char' for TBAA purposes.
  // FIXME: this should get propagated down through anonymous structs
  // and unions.
  if (mayAlias && LV.getTBAAInfo())
    LV.setTBAAInfo(CGM.getTBAAInfo(getContext().CharTy));

  return LV;
}

LValue
CodeGenFunction::EmitLValueForFieldInitialization(LValue Base,
                                                  const FieldDecl *Field) {
  QualType FieldType = Field->getType();

  if (!FieldType->isReferenceType())
    return EmitLValueForField(Base, Field);

  Address V = emitAddrOfFieldStorage(*this, Base.getAddress(), Field);

  // Make sure that the address is pointing to the right type.
  llvm::Type *llvmType = ConvertTypeForMem(FieldType);
  V = Builder.CreateElementBitCast(V, llvmType, Field->getName());

  // TODO: access-path TBAA?
  auto FieldAlignSource = getFieldAlignmentSource(Base.getAlignmentSource());
  return MakeAddrLValue(V, FieldType, FieldAlignSource);
}

LValue CodeGenFunction::EmitCompoundLiteralLValue(const CompoundLiteralExpr *E){
  if (E->isFileScope()) {
    ConstantAddress GlobalPtr = CGM.GetAddrOfConstantCompoundLiteral(E);
    return MakeAddrLValue(GlobalPtr, E->getType(), AlignmentSource::Decl);
  }
  if (E->getType()->isVariablyModifiedType())
    // make sure to emit the VLA size.
    EmitVariablyModifiedType(E->getType());

  Address DeclPtr = CreateMemTemp(E->getType(), ".compoundliteral");
  const Expr *InitExpr = E->getInitializer();
  LValue Result = MakeAddrLValue(DeclPtr, E->getType(), AlignmentSource::Decl);

  EmitAnyExprToMem(InitExpr, DeclPtr, E->getType().getQualifiers(),
                   /*Init*/ true);

  return Result;
}

LValue CodeGenFunction::EmitInitListLValue(const InitListExpr *E) {
  if (!E->isGLValue())
    // Initializing an aggregate temporary in C++11: T{...}.
    return EmitAggExprToLValue(E);

  // An lvalue initializer list must be initializing a reference.
  assert(E->getNumInits() == 1 && "reference init with multiple values");
  return EmitLValue(E->getInit(0));
}

/// Emit the operand of a glvalue conditional operator. This is either a glvalue
/// or a (possibly-parenthesized) throw-expression. If this is a throw, no
/// LValue is returned and the current block has been terminated.
static Optional<LValue> EmitLValueOrThrowExpression(CodeGenFunction &CGF,
                                                    const Expr *Operand) {
  if (auto *ThrowExpr = dyn_cast<CXXThrowExpr>(Operand->IgnoreParens())) {
    CGF.EmitCXXThrowExpr(ThrowExpr, /*KeepInsertionPoint*/false);
    return None;
  }

  return CGF.EmitLValue(Operand);
}

LValue CodeGenFunction::
EmitConditionalOperatorLValue(const AbstractConditionalOperator *expr) {
  if (!expr->isGLValue()) {
    // ?: here should be an aggregate.
    assert(hasAggregateEvaluationKind(expr->getType()) &&
           "Unexpected conditional operator!");
    return EmitAggExprToLValue(expr);
  }

  OpaqueValueMapping binding(*this, expr);

  const Expr *condExpr = expr->getCond();
  bool CondExprBool;
  if (ConstantFoldsToSimpleInteger(condExpr, CondExprBool)) {
    const Expr *live = expr->getTrueExpr(), *dead = expr->getFalseExpr();
    if (!CondExprBool) std::swap(live, dead);

    if (!ContainsLabel(dead)) {
      // If the true case is live, we need to track its region.
      if (CondExprBool)
        incrementProfileCounter(expr);
      return EmitLValue(live);
    }
  }

  llvm::BasicBlock *lhsBlock = createBasicBlock("cond.true");
  llvm::BasicBlock *rhsBlock = createBasicBlock("cond.false");
  llvm::BasicBlock *contBlock = createBasicBlock("cond.end");

  ConditionalEvaluation eval(*this);
  EmitBranchOnBoolExpr(condExpr, lhsBlock, rhsBlock, getProfileCount(expr));

  // Any temporaries created here are conditional.
  EmitBlock(lhsBlock);
  incrementProfileCounter(expr);
  eval.begin(*this);
  Optional<LValue> lhs =
      EmitLValueOrThrowExpression(*this, expr->getTrueExpr());
  eval.end(*this);

  if (lhs && !lhs->isSimple())
    return EmitUnsupportedLValue(expr, "conditional operator");

  lhsBlock = Builder.GetInsertBlock();
  if (lhs)
    Builder.CreateBr(contBlock);

  // Any temporaries created here are conditional.
  EmitBlock(rhsBlock);
  eval.begin(*this);
  Optional<LValue> rhs =
      EmitLValueOrThrowExpression(*this, expr->getFalseExpr());
  eval.end(*this);
  if (rhs && !rhs->isSimple())
    return EmitUnsupportedLValue(expr, "conditional operator");
  rhsBlock = Builder.GetInsertBlock();

  EmitBlock(contBlock);

  if (lhs && rhs) {
    llvm::PHINode *phi = Builder.CreatePHI(lhs->getPointer()->getType(),
                                           2, "cond-lvalue");
    phi->addIncoming(lhs->getPointer(), lhsBlock);
    phi->addIncoming(rhs->getPointer(), rhsBlock);
    Address result(phi, std::min(lhs->getAlignment(), rhs->getAlignment()));
    AlignmentSource alignSource =
      std::max(lhs->getAlignmentSource(), rhs->getAlignmentSource());
    return MakeAddrLValue(result, expr->getType(), alignSource);
  } else {
    assert((lhs || rhs) &&
           "both operands of glvalue conditional are throw-expressions?");
    return lhs ? *lhs : *rhs;
  }
}

/// EmitCastLValue - Casts are never lvalues unless that cast is to a reference
/// type. If the cast is to a reference, we can have the usual lvalue result,
/// otherwise if a cast is needed by the code generator in an lvalue context,
/// then it must mean that we need the address of an aggregate in order to
/// access one of its members.  This can happen for all the reasons that casts
/// are permitted with aggregate result, including noop aggregate casts, and
/// cast from scalar to union.
LValue CodeGenFunction::EmitCastLValue(const CastExpr *E) {
  switch (E->getCastKind()) {
  case CK_ToVoid:
  case CK_BitCast:
  case CK_ArrayToPointerDecay:
  case CK_FunctionToPointerDecay:
  case CK_NullToMemberPointer:
  case CK_NullToPointer:
  case CK_IntegralToPointer:
  case CK_PointerToIntegral:
  case CK_PointerToBoolean:
  case CK_VectorSplat:
  case CK_IntegralCast:
  case CK_BooleanToSignedIntegral:
  case CK_IntegralToBoolean:
  case CK_IntegralToFloating:
  case CK_FloatingToIntegral:
  case CK_FloatingToBoolean:
  case CK_FloatingCast:
  case CK_FloatingRealToComplex:
  case CK_FloatingComplexToReal:
  case CK_FloatingComplexToBoolean:
  case CK_FloatingComplexCast:
  case CK_FloatingComplexToIntegralComplex:
  case CK_IntegralRealToComplex:
  case CK_IntegralComplexToReal:
  case CK_IntegralComplexToBoolean:
  case CK_IntegralComplexCast:
  case CK_IntegralComplexToFloatingComplex:
  case CK_DerivedToBaseMemberPointer:
  case CK_BaseToDerivedMemberPointer:
  case CK_MemberPointerToBoolean:
  case CK_ReinterpretMemberPointer:
  case CK_AnyPointerToBlockPointerCast:
  case CK_ARCProduceObject:
  case CK_ARCConsumeObject:
  case CK_ARCReclaimReturnedObject:
  case CK_ARCExtendBlockObject:
  case CK_CopyAndAutoreleaseBlockObject:
  case CK_AddressSpaceConversion:
    return EmitUnsupportedLValue(E, "unexpected cast lvalue");

  case CK_Dependent:
    llvm_unreachable("dependent cast kind in IR gen!");

  case CK_BuiltinFnToFnPtr:
    llvm_unreachable("builtin functions are handled elsewhere");

  // These are never l-values; just use the aggregate emission code.
  case CK_NonAtomicToAtomic:
  case CK_AtomicToNonAtomic:
    return EmitAggExprToLValue(E);

  case CK_Dynamic: {
    LValue LV = EmitLValue(E->getSubExpr());
    Address V = LV.getAddress();
    const auto *DCE = cast<CXXDynamicCastExpr>(E);
    return MakeNaturalAlignAddrLValue(EmitDynamicCast(V, DCE), E->getType());
  }

  case CK_ConstructorConversion:
  case CK_UserDefinedConversion:
  case CK_CPointerToObjCPointerCast:
  case CK_BlockPointerToObjCPointerCast:
  case CK_NoOp:
  case CK_LValueToRValue:
    return EmitLValue(E->getSubExpr());

  case CK_UncheckedDerivedToBase:
  case CK_DerivedToBase: {
    const RecordType *DerivedClassTy =
      E->getSubExpr()->getType()->getAs<RecordType>();
    auto *DerivedClassDecl = cast<CXXRecordDecl>(DerivedClassTy->getDecl());

    LValue LV = EmitLValue(E->getSubExpr());
    Address This = LV.getAddress();

    // Perform the derived-to-base conversion
    Address Base = GetAddressOfBaseClass(
        This, DerivedClassDecl, E->path_begin(), E->path_end(),
        /*NullCheckValue=*/false, E->getExprLoc());


    return MakeAddrLValue(Base, E->getType(), LV.getAlignmentSource());
  }
  case CK_ToUnion:
    return EmitAggExprToLValue(E);
  case CK_BaseToDerived: {
    const RecordType *DerivedClassTy = E->getType()->getAs<RecordType>();
    auto *DerivedClassDecl = cast<CXXRecordDecl>(DerivedClassTy->getDecl());
    
    LValue LV = EmitLValue(E->getSubExpr());
    const CXXRecordDecl *BaseClassDecl = cast<CXXRecordDecl>(LV.getType()->getAs<RecordType>()->getDecl());

    // Perform the base-to-derived conversion
    Address Derived =
      GetAddressOfDerivedClass(LV.getAddress(), DerivedClassDecl,
                               E->path_begin(), E->path_end(),
                               /*NullCheckValue=*/false);

    if(CGM.getCodeGenOpts().EmitCastChecks && DerivedClassDecl->isPolymorphic())
      EmitVTableCastCheck(LV.getAddress(), DerivedClassDecl, BaseClassDecl);


    // C++11 [expr.static.cast]p2: Behavior is undefined if a downcast is
    // performed and the object is not of the derived type.
    if (sanitizePerformTypeCheck())
      EmitTypeCheck(TCK_DowncastReference, E->getExprLoc(),
                    Derived.getPointer(), E->getType());

    llvm::HexTypeCommonUtil HexTypeCommonUtilSet;

    // Insert HexType's type casting verification instrumentation.
    /**
    Paul: emit cast for casts with nonvirtual offset and for virtual offset.
    Here we call either EmitHexTypeCheckForCast or EmitHexTypeCheckForchangingCast*/
    if (SanOpts.has(SanitizerKind::HexType) && (!CGM.getCodeGenOpts().EmitCastChecks || !DerivedClassDecl->isPolymorphic())) {
		  if (llvm::ClCreateCastRelatedTypeList)
			  HexTypeCommonUtilSet.updateCastingReleatedTypeIntoFile(
				  ConvertType(E->getType()));
		  
		  EmitHexTypeCheckForCast(E->getType(),
		                          LV.getType(),
		                          LV.getAddress().getPointer(),
		                          /*MayBeNull=*/false,
		                          CFITCK_DerivedCast,
		                          E->getLocStart());
    }
    
    if (SanOpts.has(SanitizerKind::CFIDerivedCast))
      EmitVTablePtrCheckForCast(E->getType(), Derived.getPointer(),
                                /*MayBeNull=*/false,
                                CFITCK_DerivedCast, E->getLocStart());

    return MakeAddrLValue(Derived, E->getType(), LV.getAlignmentSource());
  }
  case CK_LValueBitCast: {
    // This must be a reinterpret_cast (or c-style equivalent).
    const auto *CE = cast<ExplicitCastExpr>(E);

    CGM.EmitExplicitCastExprType(CE, this);
    LValue LV = EmitLValue(E->getSubExpr());
    Address V = Builder.CreateBitCast(LV.getAddress(),
                                      ConvertType(CE->getTypeAsWritten()));

    if (SanOpts.has(SanitizerKind::CFIUnrelatedCast))
      EmitVTablePtrCheckForCast(E->getType(), V.getPointer(),
                                /*MayBeNull=*/false,
                                CFITCK_UnrelatedCast, E->getLocStart());

    return MakeAddrLValue(V, E->getType(), LV.getAlignmentSource());
  }
  case CK_ObjCObjectLValueCast: {
    LValue LV = EmitLValue(E->getSubExpr());
    Address V = Builder.CreateElementBitCast(LV.getAddress(),
                                             ConvertType(E->getType()));
    return MakeAddrLValue(V, E->getType(), LV.getAlignmentSource());
  }
  case CK_ZeroToOCLEvent:
    llvm_unreachable("NULL to OpenCL event lvalue cast is not valid");
  }

  llvm_unreachable("Unhandled lvalue cast kind?");
}

LValue CodeGenFunction::EmitOpaqueValueLValue(const OpaqueValueExpr *e) {
  assert(OpaqueValueMappingData::shouldBindAsLValue(e));
  return getOpaqueLValueMapping(e);
}

RValue CodeGenFunction::EmitRValueForField(LValue LV,
                                           const FieldDecl *FD,
                                           SourceLocation Loc) {
  QualType FT = FD->getType();
  LValue FieldLV = EmitLValueForField(LV, FD);
  switch (getEvaluationKind(FT)) {
  case TEK_Complex:
    return RValue::getComplex(EmitLoadOfComplex(FieldLV, Loc));
  case TEK_Aggregate:
    return FieldLV.asAggregateRValue();
  case TEK_Scalar:
    return EmitLoadOfLValue(FieldLV, Loc);
  }
  llvm_unreachable("bad evaluation kind");
}

//===--------------------------------------------------------------------===//
//                             Expression Emission
//===--------------------------------------------------------------------===//

RValue CodeGenFunction::EmitCallExpr(const CallExpr *E,
                                     ReturnValueSlot ReturnValue) {
  // Builtins never have block type.
  if (E->getCallee()->getType()->isBlockPointerType())
    return EmitBlockCallExpr(E, ReturnValue);

  if (const auto *CE = dyn_cast<CXXMemberCallExpr>(E))
    return EmitCXXMemberCallExpr(CE, ReturnValue);

  if (const auto *CE = dyn_cast<CUDAKernelCallExpr>(E))
    return EmitCUDAKernelCallExpr(CE, ReturnValue);

  const Decl *TargetDecl = E->getCalleeDecl();
  if (const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(TargetDecl)) {
    if (unsigned builtinID = FD->getBuiltinID())
      return EmitBuiltinExpr(FD, builtinID, E, ReturnValue);
  }

  if (const auto *CE = dyn_cast<CXXOperatorCallExpr>(E))
    if (const CXXMethodDecl *MD = dyn_cast_or_null<CXXMethodDecl>(TargetDecl))
      return EmitCXXOperatorMemberCallExpr(CE, MD, ReturnValue);

  if (const auto *PseudoDtor =
          dyn_cast<CXXPseudoDestructorExpr>(E->getCallee()->IgnoreParens())) {
    QualType DestroyedType = PseudoDtor->getDestroyedType();
    if (DestroyedType.hasStrongOrWeakObjCLifetime()) {
      // Automatic Reference Counting:
      //   If the pseudo-expression names a retainable object with weak or
      //   strong lifetime, the object shall be released.
      Expr *BaseExpr = PseudoDtor->getBase();
      Address BaseValue = Address::invalid();
      Qualifiers BaseQuals;

      // If this is s.x, emit s as an lvalue. If it is s->x, emit s as a scalar.
      if (PseudoDtor->isArrow()) {
        BaseValue = EmitPointerWithAlignment(BaseExpr);
        const PointerType *PTy = BaseExpr->getType()->getAs<PointerType>();
        BaseQuals = PTy->getPointeeType().getQualifiers();
      } else {
        LValue BaseLV = EmitLValue(BaseExpr);
        BaseValue = BaseLV.getAddress();
        QualType BaseTy = BaseExpr->getType();
        BaseQuals = BaseTy.getQualifiers();
      }

      switch (DestroyedType.getObjCLifetime()) {
      case Qualifiers::OCL_None:
      case Qualifiers::OCL_ExplicitNone:
      case Qualifiers::OCL_Autoreleasing:
        break;

      case Qualifiers::OCL_Strong:
        EmitARCRelease(Builder.CreateLoad(BaseValue,
                          PseudoDtor->getDestroyedType().isVolatileQualified()),
                       ARCPreciseLifetime);
        break;

      case Qualifiers::OCL_Weak:
        EmitARCDestroyWeak(BaseValue);
        break;
      }
    } else {
      // C++ [expr.pseudo]p1:
      //   The result shall only be used as the operand for the function call
      //   operator (), and the result of such a call has type void. The only
      //   effect is the evaluation of the postfix-expression before the dot or
      //   arrow.
      EmitScalarExpr(E->getCallee());
    }

    return RValue::get(nullptr);
  }

  llvm::Value *Callee = EmitScalarExpr(E->getCallee());
  return EmitCall(E->getCallee()->getType(), Callee, E, ReturnValue,
                  TargetDecl);
}

LValue CodeGenFunction::EmitBinaryOperatorLValue(const BinaryOperator *E) {
  // Comma expressions just emit their LHS then their RHS as an l-value.
  if (E->getOpcode() == BO_Comma) {
    EmitIgnoredExpr(E->getLHS());
    EnsureInsertPoint();
    return EmitLValue(E->getRHS());
  }

  if (E->getOpcode() == BO_PtrMemD ||
      E->getOpcode() == BO_PtrMemI)
    return EmitPointerToDataMemberBinaryExpr(E);

  assert(E->getOpcode() == BO_Assign && "unexpected binary l-value");

  // Note that in all of these cases, __block variables need the RHS
  // evaluated first just in case the variable gets moved by the RHS.

  switch (getEvaluationKind(E->getType())) {
  case TEK_Scalar: {
    switch (E->getLHS()->getType().getObjCLifetime()) {
    case Qualifiers::OCL_Strong:
      return EmitARCStoreStrong(E, /*ignored*/ false).first;

    case Qualifiers::OCL_Autoreleasing:
      return EmitARCStoreAutoreleasing(E).first;

    // No reason to do any of these differently.
    case Qualifiers::OCL_None:
    case Qualifiers::OCL_ExplicitNone:
    case Qualifiers::OCL_Weak:
      break;
    }

    RValue RV = EmitAnyExpr(E->getRHS());
    LValue LV = EmitCheckedLValue(E->getLHS(), TCK_Store);
    EmitStoreThroughLValue(RV, LV);
    return LV;
  }

  case TEK_Complex:
    return EmitComplexAssignmentLValue(E);

  case TEK_Aggregate:
    return EmitAggExprToLValue(E);
  }
  llvm_unreachable("bad evaluation kind");
}

LValue CodeGenFunction::EmitCallExprLValue(const CallExpr *E) {
  RValue RV = EmitCallExpr(E);

  if (!RV.isScalar())
    return MakeAddrLValue(RV.getAggregateAddress(), E->getType(),
                          AlignmentSource::Decl);

  assert(E->getCallReturnType(getContext())->isReferenceType() &&
         "Can't have a scalar return unless the return type is a "
         "reference type!");

  return MakeNaturalAlignPointeeAddrLValue(RV.getScalarVal(), E->getType());
}

LValue CodeGenFunction::EmitVAArgExprLValue(const VAArgExpr *E) {
  // FIXME: This shouldn't require another copy.
  return EmitAggExprToLValue(E);
}

LValue CodeGenFunction::EmitCXXConstructLValue(const CXXConstructExpr *E) {
  assert(E->getType()->getAsCXXRecordDecl()->hasTrivialDestructor()
         && "binding l-value to type which needs a temporary");
  AggValueSlot Slot = CreateAggTemp(E->getType());
  EmitCXXConstructExpr(E, Slot);
  return MakeAddrLValue(Slot.getAddress(), E->getType(),
                        AlignmentSource::Decl);
}

LValue
CodeGenFunction::EmitCXXTypeidLValue(const CXXTypeidExpr *E) {
  return MakeNaturalAlignAddrLValue(EmitCXXTypeidExpr(E), E->getType());
}

Address CodeGenFunction::EmitCXXUuidofExpr(const CXXUuidofExpr *E) {
  return Builder.CreateElementBitCast(CGM.GetAddrOfUuidDescriptor(E),
                                      ConvertType(E->getType()));
}

LValue CodeGenFunction::EmitCXXUuidofLValue(const CXXUuidofExpr *E) {
  return MakeAddrLValue(EmitCXXUuidofExpr(E), E->getType(),
                        AlignmentSource::Decl);
}

LValue
CodeGenFunction::EmitCXXBindTemporaryLValue(const CXXBindTemporaryExpr *E) {
  AggValueSlot Slot = CreateAggTemp(E->getType(), "temp.lvalue");
  Slot.setExternallyDestructed();
  EmitAggExpr(E->getSubExpr(), Slot);
  EmitCXXTemporary(E->getTemporary(), E->getType(), Slot.getAddress());
  return MakeAddrLValue(Slot.getAddress(), E->getType(),
                        AlignmentSource::Decl);
}

LValue
CodeGenFunction::EmitLambdaLValue(const LambdaExpr *E) {
  AggValueSlot Slot = CreateAggTemp(E->getType(), "temp.lvalue");
  EmitLambdaExpr(E, Slot);
  return MakeAddrLValue(Slot.getAddress(), E->getType(),
                        AlignmentSource::Decl);
}

LValue CodeGenFunction::EmitObjCMessageExprLValue(const ObjCMessageExpr *E) {
  RValue RV = EmitObjCMessageExpr(E);

  if (!RV.isScalar())
    return MakeAddrLValue(RV.getAggregateAddress(), E->getType(),
                          AlignmentSource::Decl);

  assert(E->getMethodDecl()->getReturnType()->isReferenceType() &&
         "Can't have a scalar return unless the return type is a "
         "reference type!");

  return MakeNaturalAlignPointeeAddrLValue(RV.getScalarVal(), E->getType());
}

LValue CodeGenFunction::EmitObjCSelectorLValue(const ObjCSelectorExpr *E) {
  Address V =
    CGM.getObjCRuntime().GetAddrOfSelector(*this, E->getSelector());
  return MakeAddrLValue(V, E->getType(), AlignmentSource::Decl);
}

llvm::Value *CodeGenFunction::EmitIvarOffset(const ObjCInterfaceDecl *Interface,
                                             const ObjCIvarDecl *Ivar) {
  return CGM.getObjCRuntime().EmitIvarOffset(*this, Interface, Ivar);
}

LValue CodeGenFunction::EmitLValueForIvar(QualType ObjectTy,
                                          llvm::Value *BaseValue,
                                          const ObjCIvarDecl *Ivar,
                                          unsigned CVRQualifiers) {
  return CGM.getObjCRuntime().EmitObjCValueForIvar(*this, ObjectTy, BaseValue,
                                                   Ivar, CVRQualifiers);
}

LValue CodeGenFunction::EmitObjCIvarRefLValue(const ObjCIvarRefExpr *E) {
  // FIXME: A lot of the code below could be shared with EmitMemberExpr.
  llvm::Value *BaseValue = nullptr;
  const Expr *BaseExpr = E->getBase();
  Qualifiers BaseQuals;
  QualType ObjectTy;
  if (E->isArrow()) {
    BaseValue = EmitScalarExpr(BaseExpr);
    ObjectTy = BaseExpr->getType()->getPointeeType();
    BaseQuals = ObjectTy.getQualifiers();
  } else {
    LValue BaseLV = EmitLValue(BaseExpr);
    BaseValue = BaseLV.getPointer();
    ObjectTy = BaseExpr->getType();
    BaseQuals = ObjectTy.getQualifiers();
  }

  LValue LV =
    EmitLValueForIvar(ObjectTy, BaseValue, E->getDecl(),
                      BaseQuals.getCVRQualifiers());
  setObjCGCLValueClass(getContext(), E, LV);
  return LV;
}

LValue CodeGenFunction::EmitStmtExprLValue(const StmtExpr *E) {
  // Can only get l-value for message expression returning aggregate type
  RValue RV = EmitAnyExprToTemp(E);
  return MakeAddrLValue(RV.getAggregateAddress(), E->getType(),
                        AlignmentSource::Decl);
}

RValue CodeGenFunction::EmitCall(QualType CalleeType, llvm::Value *Callee,
                                 const CallExpr *E, ReturnValueSlot ReturnValue,
                                 CGCalleeInfo CalleeInfo, llvm::Value *Chain) {
  // Get the actual function type. The callee type will always be a pointer to
  // function type or a block pointer type.
  assert(CalleeType->isFunctionPointerType() &&
         "Call must have function pointer type!");

  // Preserve the non-canonical function type because things like exception
  // specifications disappear in the canonical type. That information is useful
  // to drive the generation of more accurate code for this call later on.
  const FunctionProtoType *NonCanonicalFTP = CalleeType->getAs<PointerType>()
                                                 ->getPointeeType()
                                                 ->getAs<FunctionProtoType>();

  const Decl *TargetDecl = CalleeInfo.getCalleeDecl();

  if (const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(TargetDecl))
    // We can only guarantee that a function is called from the correct
    // context/function based on the appropriate target attributes,
    // so only check in the case where we have both always_inline and target
    // since otherwise we could be making a conditional call after a check for
    // the proper cpu features (and it won't cause code generation issues due to
    // function based code generation).
    if (TargetDecl->hasAttr<AlwaysInlineAttr>() &&
        TargetDecl->hasAttr<TargetAttr>())
      checkTargetFeatures(E, FD);

  CalleeType = getContext().getCanonicalType(CalleeType);

  const auto *FnType =
      cast<FunctionType>(cast<PointerType>(CalleeType)->getPointeeType());

  if (getLangOpts().CPlusPlus && SanOpts.has(SanitizerKind::Function) &&
      (!TargetDecl || !isa<FunctionDecl>(TargetDecl))) {
    if (llvm::Constant *PrefixSig =
            CGM.getTargetCodeGenInfo().getUBSanFunctionSignature(CGM)) {
      SanitizerScope SanScope(this);
      llvm::Constant *FTRTTIConst =
          CGM.GetAddrOfRTTIDescriptor(QualType(FnType, 0), /*ForEH=*/true);
      llvm::Type *PrefixStructTyElems[] = {
        PrefixSig->getType(),
        FTRTTIConst->getType()
      };
      llvm::StructType *PrefixStructTy = llvm::StructType::get(
          CGM.getLLVMContext(), PrefixStructTyElems, /*isPacked=*/true);

      llvm::Value *CalleePrefixStruct = Builder.CreateBitCast(
          Callee, llvm::PointerType::getUnqual(PrefixStructTy));
      llvm::Value *CalleeSigPtr =
          Builder.CreateConstGEP2_32(PrefixStructTy, CalleePrefixStruct, 0, 0);
      llvm::Value *CalleeSig =
          Builder.CreateAlignedLoad(CalleeSigPtr, getIntAlign());
      llvm::Value *CalleeSigMatch = Builder.CreateICmpEQ(CalleeSig, PrefixSig);

      llvm::BasicBlock *Cont = createBasicBlock("cont");
      llvm::BasicBlock *TypeCheck = createBasicBlock("typecheck");
      Builder.CreateCondBr(CalleeSigMatch, TypeCheck, Cont);

      EmitBlock(TypeCheck);
      llvm::Value *CalleeRTTIPtr =
          Builder.CreateConstGEP2_32(PrefixStructTy, CalleePrefixStruct, 0, 1);
      llvm::Value *CalleeRTTI =
          Builder.CreateAlignedLoad(CalleeRTTIPtr, getPointerAlign());
      llvm::Value *CalleeRTTIMatch =
          Builder.CreateICmpEQ(CalleeRTTI, FTRTTIConst);
      llvm::Constant *StaticData[] = {
        EmitCheckSourceLocation(E->getLocStart()),
        EmitCheckTypeDescriptor(CalleeType)
      };
      EmitCheck(std::make_pair(CalleeRTTIMatch, SanitizerKind::Function),
                "function_type_mismatch", StaticData, Callee);

      Builder.CreateBr(Cont);
      EmitBlock(Cont);
    }
  }

  // If we are checking indirect calls and this call is indirect, check that the
  // function pointer is a member of the bit set for the function type.
  if (SanOpts.has(SanitizerKind::CFIICall) &&
      (!TargetDecl || !isa<FunctionDecl>(TargetDecl))) {
    SanitizerScope SanScope(this);
    EmitSanitizerStatReport(llvm::SanStat_CFI_ICall);

    llvm::Metadata *MD = CGM.CreateMetadataIdentifierForType(QualType(FnType, 0));
    llvm::Value *BitSetName = llvm::MetadataAsValue::get(getLLVMContext(), MD);

    llvm::Value *CastedCallee = Builder.CreateBitCast(Callee, Int8PtrTy);
    llvm::Value *BitSetTest =
        Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::bitset_test),
                           {CastedCallee, BitSetName});

    auto TypeId = CGM.CreateCfiIdForTypeMetadata(MD);
    llvm::Constant *StaticData[] = {
        llvm::ConstantInt::get(Int8Ty, CFITCK_ICall),
        EmitCheckSourceLocation(E->getLocStart()),
        EmitCheckTypeDescriptor(QualType(FnType, 0)),
    };
    if (CGM.getCodeGenOpts().SanitizeCfiCrossDso && TypeId) {
      EmitCfiSlowPathCheck(SanitizerKind::CFIICall, BitSetTest, TypeId,
                           CastedCallee, StaticData);
    } else {
      EmitCheck(std::make_pair(BitSetTest, SanitizerKind::CFIICall),
                "cfi_check_fail", StaticData,
                {CastedCallee, llvm::UndefValue::get(IntPtrTy)});
    }
  }

  CallArgList Args;
  if (Chain)
    Args.add(RValue::get(Builder.CreateBitCast(Chain, CGM.VoidPtrTy)),
             CGM.getContext().VoidPtrTy);
  EmitCallArgs(Args, dyn_cast<FunctionProtoType>(FnType), E->arguments(),
               E->getDirectCallee(), /*ParamsToSkip*/ 0);

  const CGFunctionInfo &FnInfo = CGM.getTypes().arrangeFreeFunctionCall(
      Args, FnType, /*isChainCall=*/Chain);

  // C99 6.5.2.2p6:
  //   If the expression that denotes the called function has a type
  //   that does not include a prototype, [the default argument
  //   promotions are performed]. If the number of arguments does not
  //   equal the number of parameters, the behavior is undefined. If
  //   the function is defined with a type that includes a prototype,
  //   and either the prototype ends with an ellipsis (, ...) or the
  //   types of the arguments after promotion are not compatible with
  //   the types of the parameters, the behavior is undefined. If the
  //   function is defined with a type that does not include a
  //   prototype, and the types of the arguments after promotion are
  //   not compatible with those of the parameters after promotion,
  //   the behavior is undefined [except in some trivial cases].
  // That is, in the general case, we should assume that a call
  // through an unprototyped function type works like a *non-variadic*
  // call.  The way we make this work is to cast to the exact type
  // of the promoted arguments.
  //
  // Chain calls use this same code path to add the invisible chain parameter
  // to the function type.
  if (isa<FunctionNoProtoType>(FnType) || Chain) {
    llvm::Type *CalleeTy = getTypes().GetFunctionType(FnInfo);
    CalleeTy = CalleeTy->getPointerTo();
    Callee = Builder.CreateBitCast(Callee, CalleeTy, "callee.knr.cast");
  }

  return EmitCall(FnInfo, Callee, ReturnValue, Args,
                  CGCalleeInfo(NonCanonicalFTP, TargetDecl));
}

LValue CodeGenFunction::
EmitPointerToDataMemberBinaryExpr(const BinaryOperator *E) {
  Address BaseAddr = Address::invalid();
  if (E->getOpcode() == BO_PtrMemI) {
    BaseAddr = EmitPointerWithAlignment(E->getLHS());
  } else {
    BaseAddr = EmitLValue(E->getLHS()).getAddress();
  }

  llvm::Value *OffsetV = EmitScalarExpr(E->getRHS());

  const MemberPointerType *MPT
    = E->getRHS()->getType()->getAs<MemberPointerType>();

  AlignmentSource AlignSource;
  Address MemberAddr =
    EmitCXXMemberDataPointerAddress(E, BaseAddr, OffsetV, MPT,
                                    &AlignSource);

  return MakeAddrLValue(MemberAddr, MPT->getPointeeType(), AlignSource);
}

/// Given the address of a temporary variable, produce an r-value of
/// its type.
RValue CodeGenFunction::convertTempToRValue(Address addr,
                                            QualType type,
                                            SourceLocation loc) {
  LValue lvalue = MakeAddrLValue(addr, type, AlignmentSource::Decl);
  switch (getEvaluationKind(type)) {
  case TEK_Complex:
    return RValue::getComplex(EmitLoadOfComplex(lvalue, loc));
  case TEK_Aggregate:
    return lvalue.asAggregateRValue();
  case TEK_Scalar:
    return RValue::get(EmitLoadOfScalar(lvalue, loc));
  }
  llvm_unreachable("bad evaluation kind");
}

void CodeGenFunction::SetFPAccuracy(llvm::Value *Val, float Accuracy) {
  assert(Val->getType()->isFPOrFPVectorTy());
  if (Accuracy == 0.0 || !isa<llvm::Instruction>(Val))
    return;

  llvm::MDBuilder MDHelper(getLLVMContext());
  llvm::MDNode *Node = MDHelper.createFPMath(Accuracy);

  cast<llvm::Instruction>(Val)->setMetadata(llvm::LLVMContext::MD_fpmath, Node);
}

namespace {
  struct LValueOrRValue {
    LValue LV;
    RValue RV;
  };
}

static LValueOrRValue emitPseudoObjectExpr(CodeGenFunction &CGF,
                                           const PseudoObjectExpr *E,
                                           bool forLValue,
                                           AggValueSlot slot) {
  SmallVector<CodeGenFunction::OpaqueValueMappingData, 4> opaques;

  // Find the result expression, if any.
  const Expr *resultExpr = E->getResultExpr();
  LValueOrRValue result;

  for (PseudoObjectExpr::const_semantics_iterator
         i = E->semantics_begin(), e = E->semantics_end(); i != e; ++i) {
    const Expr *semantic = *i;

    // If this semantic expression is an opaque value, bind it
    // to the result of its source expression.
    if (const auto *ov = dyn_cast<OpaqueValueExpr>(semantic)) {

      // If this is the result expression, we may need to evaluate
      // directly into the slot.
      typedef CodeGenFunction::OpaqueValueMappingData OVMA;
      OVMA opaqueData;
      if (ov == resultExpr && ov->isRValue() && !forLValue &&
          CodeGenFunction::hasAggregateEvaluationKind(ov->getType())) {
        CGF.EmitAggExpr(ov->getSourceExpr(), slot);

        LValue LV = CGF.MakeAddrLValue(slot.getAddress(), ov->getType(),
                                       AlignmentSource::Decl);
        opaqueData = OVMA::bind(CGF, ov, LV);
        result.RV = slot.asRValue();

      // Otherwise, emit as normal.
      } else {
        opaqueData = OVMA::bind(CGF, ov, ov->getSourceExpr());

        // If this is the result, also evaluate the result now.
        if (ov == resultExpr) {
          if (forLValue)
            result.LV = CGF.EmitLValue(ov);
          else
            result.RV = CGF.EmitAnyExpr(ov, slot);
        }
      }

      opaques.push_back(opaqueData);

    // Otherwise, if the expression is the result, evaluate it
    // and remember the result.
    } else if (semantic == resultExpr) {
      if (forLValue)
        result.LV = CGF.EmitLValue(semantic);
      else
        result.RV = CGF.EmitAnyExpr(semantic, slot);

    // Otherwise, evaluate the expression in an ignored context.
    } else {
      CGF.EmitIgnoredExpr(semantic);
    }
  }

  // Unbind all the opaques now.
  for (unsigned i = 0, e = opaques.size(); i != e; ++i)
    opaques[i].unbind(CGF);

  return result;
}

RValue CodeGenFunction::EmitPseudoObjectRValue(const PseudoObjectExpr *E,
                                               AggValueSlot slot) {
  return emitPseudoObjectExpr(*this, E, false, slot).RV;
}

LValue CodeGenFunction::EmitPseudoObjectLValue(const PseudoObjectExpr *E) {
  return emitPseudoObjectExpr(*this, E, true, AggValueSlot::ignored()).LV;
}
