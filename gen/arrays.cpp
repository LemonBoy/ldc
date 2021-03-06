//===-- arrays.cpp --------------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "gen/arrays.h"
#include "aggregate.h"
#include "declaration.h"
#include "dsymbol.h"
#include "expression.h"
#include "init.h"
#include "module.h"
#include "mtype.h"
#include "gen/dvalue.h"
#include "gen/funcgenstate.h"
#include "gen/irstate.h"
#include "gen/llvm.h"
#include "gen/llvmhelpers.h"
#include "gen/logger.h"
#include "gen/runtime.h"
#include "gen/tollvm.h"
#include "ir/irfunction.h"
#include "ir/irmodule.h"

static void DtoSetArray(DValue *array, LLValue *dim, LLValue *ptr);

////////////////////////////////////////////////////////////////////////////////

namespace {
LLValue *DtoSlice(LLValue *ptr, LLValue *length, LLType *elemType = nullptr) {
  if (!elemType)
    elemType = ptr->getType()->getContainedType(0);
  elemType = i1ToI8(voidToI8(elemType));
  return DtoAggrPair(length, DtoBitCast(ptr, elemType->getPointerTo()));
}

LLValue *DtoSlice(Expression *e) {
  DValue *dval = toElem(e);
  if (dval->type->toBasetype()->ty == Tsarray) {
    // Convert static array to slice
    return DtoSlice(DtoLVal(dval), DtoArrayLen(dval));
  }
  return DtoRVal(dval);
}
}

////////////////////////////////////////////////////////////////////////////////

static LLValue *DtoSlicePtr(Expression *e) {
  DValue *dval = toElem(e);
  Loc loc;
  LLStructType *type = DtoArrayType(LLType::getInt8Ty(gIR->context()));
  Type *vt = dval->type->toBasetype();
  if (vt->ty == Tarray) {
    return makeLValue(loc, dval);
  }

  bool isStaticArray = vt->ty == Tsarray;
  LLValue *val = isStaticArray ? DtoLVal(dval) : makeLValue(loc, dval);
  LLValue *array = DtoRawAlloca(type, 0, ".array");
  LLValue *len = isStaticArray ? DtoArrayLen(dval) : DtoConstSize_t(1);
  DtoStore(len, DtoGEPi(array, 0, 0));
  DtoStore(DtoBitCast(val, getVoidPtrType()), DtoGEPi(array, 0, 1));
  return array;
}

////////////////////////////////////////////////////////////////////////////////

LLStructType *DtoArrayType(Type *arrayTy) {
  assert(arrayTy->nextOf());
  llvm::Type *elems[] = {DtoSize_t(), DtoPtrToType(arrayTy->nextOf())};
  return llvm::StructType::get(gIR->context(), elems, false);
}

LLStructType *DtoArrayType(LLType *t) {
  llvm::Type *elems[] = {DtoSize_t(), getPtrToType(t)};
  return llvm::StructType::get(gIR->context(), elems, false);
}

////////////////////////////////////////////////////////////////////////////////

LLArrayType *DtoStaticArrayType(Type *t) {
  t = t->toBasetype();
  assert(t->ty == Tsarray);
  TypeSArray *tsa = static_cast<TypeSArray *>(t);
  Type *tnext = tsa->nextOf();

  return LLArrayType::get(DtoMemType(tnext), tsa->dim->toUInteger());
}

////////////////////////////////////////////////////////////////////////////////

void DtoSetArrayToNull(LLValue *v) {
  IF_LOG Logger::println("DtoSetArrayToNull");
  LOG_SCOPE;

  assert(isaPointer(v));
  LLType *t = v->getType()->getContainedType(0);

  DtoStore(LLConstant::getNullValue(t), v);
}

////////////////////////////////////////////////////////////////////////////////

static void DtoArrayInit(Loc &loc, LLValue *ptr, LLValue *length,
                         DValue *dvalue) {
  IF_LOG Logger::println("DtoArrayInit");
  LOG_SCOPE;

  // lets first optimize all zero/constant i8 initializations down to a memset.
  // this simplifies codegen later on as llvm null's have no address!
  if (!dvalue->isLVal()) {
    LLConstant *constantVal = isaConstant(DtoRVal(dvalue));
    if (constantVal &&
        (constantVal->isNullValue() ||
         constantVal->getType() == LLType::getInt8Ty(gIR->context()))) {
      LLValue *size = length;
      size_t elementSize = getTypeAllocSize(constantVal->getType());
      if (elementSize != 1) {
        size = gIR->ir->CreateMul(length, DtoConstSize_t(elementSize),
                                  ".arraysize");
      }
      if (constantVal->isNullValue()) {
        DtoMemSetZero(ptr, size);
      } else {
        DtoMemSet(ptr, constantVal, size);
      }
      return;
    }
  }

  // create blocks
  llvm::BasicBlock *condbb = gIR->insertBB("arrayinit.cond");
  llvm::BasicBlock *bodybb = gIR->insertBBAfter(condbb, "arrayinit.body");
  llvm::BasicBlock *endbb = gIR->insertBBAfter(bodybb, "arrayinit.end");

  // initialize iterator
  LLValue *itr = DtoAllocaDump(DtoConstSize_t(0), 0, "arrayinit.itr");

  // move into the for condition block, ie. start the loop
  assert(!gIR->scopereturned());
  llvm::BranchInst::Create(condbb, gIR->scopebb());

  // replace current scope
  gIR->scope() = IRScope(condbb);

  // create the condition
  LLValue *cond_val =
      gIR->ir->CreateICmpNE(DtoLoad(itr), length, "arrayinit.condition");

  // conditional branch
  assert(!gIR->scopereturned());
  llvm::BranchInst::Create(bodybb, endbb, cond_val, gIR->scopebb());

  // rewrite scope
  gIR->scope() = IRScope(bodybb);

  LLValue *itr_val = DtoLoad(itr);
  // assign array element value
  DLValue arrayelem(dvalue->type->toBasetype(),
                    DtoGEP1(ptr, itr_val, true, "arrayinit.arrayelem"));
  DtoAssign(loc, &arrayelem, dvalue, TOKblit);

  // increment iterator
  DtoStore(gIR->ir->CreateAdd(itr_val, DtoConstSize_t(1), "arrayinit.new_itr"),
           itr);

  // loop
  llvm::BranchInst::Create(condbb, gIR->scopebb());

  // rewrite the scope
  gIR->scope() = IRScope(endbb);
}

////////////////////////////////////////////////////////////////////////////////

static Type *DtoArrayElementType(Type *arrayType) {
  assert(arrayType->toBasetype()->nextOf());
  Type *t = arrayType->toBasetype()->nextOf()->toBasetype();
  while (t->ty == Tsarray) {
    t = t->nextOf()->toBasetype();
  }
  return t;
}

////////////////////////////////////////////////////////////////////////////////

static void copySlice(Loc &loc, LLValue *dstarr, LLValue *sz1, LLValue *srcarr,
                      LLValue *sz2, bool knownInBounds) {
  const bool checksEnabled =
      global.params.useAssert || gIR->emitArrayBoundsChecks();
  if (checksEnabled && !knownInBounds) {
    LLValue *fn = getRuntimeFunction(loc, gIR->module, "_d_array_slice_copy");
    gIR->CreateCallOrInvoke(fn, dstarr, sz1, srcarr, sz2);
  } else {
    // We might have dstarr == srcarr at compile time, but as long as
    // sz1 == 0 at runtime, this would probably still be legal (the C spec
    // is unclear here).
    DtoMemCpy(dstarr, srcarr, sz1);
  }
}

////////////////////////////////////////////////////////////////////////////////

// Determine whether t is an array of structs that need a postblit.
static bool arrayNeedsPostblit(Type *t) {
  t = DtoArrayElementType(t);
  if (t->ty == Tstruct) {
    return static_cast<TypeStruct *>(t)->sym->postblit != nullptr;
  }
  return false;
}

// Does array assignment (or initialization) from another array of the same
// element type or from an appropriate single element.
void DtoArrayAssign(Loc &loc, DValue *lhs, DValue *rhs, int op,
                    bool canSkipPostblit) {
  IF_LOG Logger::println("DtoArrayAssign");
  LOG_SCOPE;

  Type *t = lhs->type->toBasetype();
  Type *t2 = rhs->type->toBasetype();
  assert(t->nextOf());

  // reference assignment for dynamic array?
  if (t->ty == Tarray && !lhs->isSlice()) {
    assert(t2->ty == Tarray || t2->ty == Tsarray);
    if (rhs->isNull()) {
      DtoSetArrayToNull(DtoLVal(lhs));
    } else {
      DtoSetArray(lhs, DtoArrayLen(rhs), DtoArrayPtr(rhs));
    }
    return;
  }

  // TOKblit is generated by the frontend for (default) initialization of
  // static arrays of structs with a single element.
  const bool isConstructing = (op == TOKconstruct || op == TOKblit);

  Type *const elemType = t->nextOf()->toBasetype();
  const bool needsDestruction =
      (!isConstructing && elemType->needsDestruction());
  LLValue *realLhsPtr = DtoArrayPtr(lhs);
  LLValue *lhsPtr = DtoBitCast(realLhsPtr, getVoidPtrType());
  LLValue *lhsLength = DtoArrayLen(lhs);

  // Be careful to handle void arrays correctly when modifying this (see tests
  // for DMD issue 7493).
  // TODO: This should use AssignExp::memset.
  LLValue *realRhsArrayPtr =
      (t2->ty == Tarray || t2->ty == Tsarray) ? DtoArrayPtr(rhs) : nullptr;
  if (realRhsArrayPtr && realRhsArrayPtr->getType() == realLhsPtr->getType()) {
    // T[]  = T[]      T[]  = T[n]
    // T[n] = T[n]     T[n] = T[]
    LLValue *rhsPtr = DtoBitCast(realRhsArrayPtr, getVoidPtrType());
    LLValue *rhsLength = DtoArrayLen(rhs);

    const bool needsPostblit = (op != TOKblit && arrayNeedsPostblit(t) &&
                                (!canSkipPostblit || t2->ty == Tarray));

    if (!needsDestruction && !needsPostblit) {
      // fast version
      LLValue *elemSize =
          DtoConstSize_t(getTypeAllocSize(DtoMemType(elemType)));
      LLValue *lhsSize = gIR->ir->CreateMul(elemSize, lhsLength);

      if (rhs->isNull()) {
        DtoMemSetZero(lhsPtr, lhsSize);
      } else {
        LLValue *rhsSize = gIR->ir->CreateMul(elemSize, rhsLength);
        const bool knownInBounds =
            isConstructing || (t->ty == Tsarray && t2->ty == Tsarray);
        copySlice(loc, lhsPtr, lhsSize, rhsPtr, rhsSize, knownInBounds);
      }
    } else if (isConstructing) {
      LLFunction *fn = getRuntimeFunction(loc, gIR->module, "_d_arrayctor");
      LLCallSite call = gIR->CreateCallOrInvoke(fn, DtoTypeInfoOf(elemType),
                                                DtoSlice(rhsPtr, rhsLength),
                                                DtoSlice(lhsPtr, lhsLength));
      call.setCallingConv(llvm::CallingConv::C);
    } else // assigning
    {
      LLValue *tmpSwap = DtoAlloca(elemType, "arrayAssign.tmpSwap");
      LLFunction *fn = getRuntimeFunction(
          loc, gIR->module,
          !canSkipPostblit ? "_d_arrayassign_l" : "_d_arrayassign_r");
      LLCallSite call = gIR->CreateCallOrInvoke(
          fn, DtoTypeInfoOf(elemType), DtoSlice(rhsPtr, rhsLength),
          DtoSlice(lhsPtr, lhsLength), DtoBitCast(tmpSwap, getVoidPtrType()));
      call.setCallingConv(llvm::CallingConv::C);
    }
  } else {
    // scalar rhs:
    // T[]  = T     T[n][]  = T
    // T[n] = T     T[n][m] = T
    const bool needsPostblit =
        (op != TOKblit && !canSkipPostblit && arrayNeedsPostblit(t));

    if (!needsDestruction && !needsPostblit) {
      // fast version
      LLValue *elemSize = DtoConstSize_t(
          getTypeAllocSize(realLhsPtr->getType()->getContainedType(0)));
      LLValue *lhsSize = gIR->ir->CreateMul(elemSize, lhsLength);
      LLType *rhsType = DtoMemType(t2);
      LLValue *rhsSize = DtoConstSize_t(getTypeAllocSize(rhsType));
      LLValue *actualPtr = DtoBitCast(lhsPtr, rhsType->getPointerTo());
      LLValue *actualLength = gIR->ir->CreateExactUDiv(lhsSize, rhsSize);
      DtoArrayInit(loc, actualPtr, actualLength, rhs);
    } else {
      LLFunction *fn = getRuntimeFunction(loc, gIR->module,
                                          isConstructing ? "_d_arraysetctor"
                                                         : "_d_arraysetassign");
      LLCallSite call = gIR->CreateCallOrInvoke(
          fn, lhsPtr, DtoBitCast(makeLValue(loc, rhs), getVoidPtrType()),
          gIR->ir->CreateTruncOrBitCast(lhsLength,
                                        LLType::getInt32Ty(gIR->context())),
          DtoTypeInfoOf(stripModifiers(t2)));
      call.setCallingConv(llvm::CallingConv::C);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

static void DtoSetArray(DValue *array, LLValue *dim, LLValue *ptr) {
  IF_LOG Logger::println("SetArray");
  LLValue *arr = DtoLVal(array);
  assert(isaStruct(arr->getType()->getContainedType(0)));
  DtoStore(dim, DtoGEPi(arr, 0, 0));
  DtoStore(ptr, DtoGEPi(arr, 0, 1));
}

////////////////////////////////////////////////////////////////////////////////

LLConstant *DtoConstArrayInitializer(ArrayInitializer *arrinit,
                                     Type *targetType) {
  IF_LOG Logger::println("DtoConstArrayInitializer: %s | %s",
                         arrinit->toChars(), targetType->toChars());
  LOG_SCOPE;

  assert(arrinit->value.dim == arrinit->index.dim);

  // get base array type
  Type *arrty = targetType->toBasetype();
  size_t arrlen = arrinit->dim;

  // for statis arrays, dmd does not include any trailing default
  // initialized elements in the value/index lists
  if (arrty->ty == Tsarray) {
    TypeSArray *tsa = static_cast<TypeSArray *>(arrty);
    arrlen = static_cast<size_t>(tsa->dim->toInteger());
  }

  // make sure the number of initializers is sane
  if (arrinit->index.dim > arrlen || arrinit->dim > arrlen) {
    error(arrinit->loc, "too many initializers, %llu, for array[%llu]",
          static_cast<unsigned long long>(arrinit->index.dim),
          static_cast<unsigned long long>(arrlen));
    fatal();
  }

  // get elem type
  Type *elemty;
  if (arrty->ty == Tvector) {
    elemty = static_cast<TypeVector *>(arrty)->elementType();
  } else {
    elemty = arrty->nextOf();
  }
  LLType *llelemty = DtoMemType(elemty);

  // true if array elements differ in type, can happen with array of unions
  bool mismatch = false;

  // allocate room for initializers
  std::vector<LLConstant *> initvals(arrlen, nullptr);

  // go through each initializer, they're not sorted by index by the frontend
  size_t j = 0;
  for (size_t i = 0; i < arrinit->index.dim; i++) {
    // get index
    Expression *idx = static_cast<Expression *>(arrinit->index.data[i]);

    // idx can be null, then it's just the next element
    if (idx) {
      j = idx->toInteger();
    }
    assert(j < arrlen);

    // get value
    Initializer *val = static_cast<Initializer *>(arrinit->value.data[i]);
    assert(val);

    // error check from dmd
    if (initvals[j] != nullptr) {
      error(arrinit->loc, "duplicate initialization for index %llu",
            static_cast<unsigned long long>(j));
    }

    LLConstant *c = DtoConstInitializer(val->loc, elemty, val);
    assert(c);
    if (c->getType() != llelemty) {
      mismatch = true;
    }

    initvals[j] = c;
    j++;
  }

  // die now if there was errors
  if (global.errors) {
    fatal();
  }

  // Fill out any null entries still left with default values.

  // Element default initializer. Compute lazily to be able to avoid infinite
  // recursion for types with members that are default initialized to empty
  // arrays of themselves.
  LLConstant *elemDefaultInit = nullptr;
  for (size_t i = 0; i < arrlen; i++) {
    if (initvals[i] != nullptr) {
      continue;
    }

    if (!elemDefaultInit) {
      elemDefaultInit = DtoConstExpInit(arrinit->loc, elemty,
                                        elemty->defaultInit(arrinit->loc));
      if (elemDefaultInit->getType() != llelemty) {
        mismatch = true;
      }
    }

    initvals[i] = elemDefaultInit;
  }

  LLConstant *constarr;
  if (mismatch) {
    constarr = LLConstantStruct::getAnon(gIR->context(),
                                         initvals); // FIXME should this pack?
  } else {
    if (arrty->ty == Tvector) {
      constarr = llvm::ConstantVector::get(initvals);
    } else {
      constarr =
          LLConstantArray::get(LLArrayType::get(llelemty, arrlen), initvals);
    }
  }

  //     std::cout << "constarr: " << *constarr << std::endl;

  // if the type is a static array, we're done
  if (arrty->ty == Tsarray || arrty->ty == Tvector) {
    return constarr;
  }

  // we need to make a global with the data, so we have a pointer to the array
  // Important: don't make the gvar constant, since this const initializer might
  // be used as an initializer for a static T[] - where modifying contents is
  // allowed.
  auto gvar = new LLGlobalVariable(gIR->module, constarr->getType(), false,
                                   LLGlobalValue::InternalLinkage, constarr,
                                   ".constarray");

  if (arrty->ty == Tpointer) {
    // we need to return pointer to the static array.
    return DtoBitCast(gvar, DtoType(arrty));
  }

  LLConstant *idxs[2] = {DtoConstUint(0), DtoConstUint(0)};

#if LDC_LLVM_VER >= 307
  LLConstant *gep = llvm::ConstantExpr::getGetElementPtr(
      isaPointer(gvar)->getElementType(), gvar, idxs, true);
#else
  LLConstant *gep = llvm::ConstantExpr::getGetElementPtr(gvar, idxs, true);
#endif
  gep = llvm::ConstantExpr::getBitCast(gvar, getPtrToType(llelemty));

  return DtoConstSlice(DtoConstSize_t(arrlen), gep, arrty);
}

////////////////////////////////////////////////////////////////////////////////

Expression *indexArrayLiteral(ArrayLiteralExp *ale, unsigned idx) {
  assert(idx < ale->elements->dim);
  auto e = (*ale->elements)[idx];
  if (!e) {
    return ale->basis;
  }
  return e;
}

////////////////////////////////////////////////////////////////////////////////

bool isConstLiteral(Expression *e) {
  // We have to check the return value of isConst specifically for '1',
  // as SymOffExp is classified as '2' and the address of a local variable is
  // not an LLVM constant.
  //
  // Examine the ArrayLiteralExps and the StructLiteralExps element by element
  // as isConst always returns 0 on those.
  switch (e->op) {
  case TOKarrayliteral: {
    auto ale = static_cast<ArrayLiteralExp *>(e);
    for (auto el : *ale->elements) {
      if (!isConstLiteral(el ? el : ale->basis))
        return false;
    }
  } break;

  case TOKstructliteral: {
    auto sle = static_cast<StructLiteralExp *>(e);
    if (sle->sd->isNested())
      return false;
    for (auto el : *sle->elements) {
      if (el && !isConstLiteral(el))
        return false;
    }
  } break;

  default:
    if (e->isConst() != 1)
      return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////

llvm::Constant *arrayLiteralToConst(IRState *p, ArrayLiteralExp *ale) {
  // Build the initializer. We have to take care as due to unions in the
  // element types (with different fields being initialized), we can end up
  // with different types for the initializer values. In this case, we
  // generate a packed struct constant instead of an array constant.
  LLType *elementType = nullptr;
  bool differentTypes = false;

  std::vector<LLConstant *> vals;
  vals.reserve(ale->elements->dim);
  for (unsigned i = 0; i < ale->elements->dim; ++i) {
    llvm::Constant *val = toConstElem(indexArrayLiteral(ale, i), p);
    if (!elementType) {
      elementType = val->getType();
    } else {
      differentTypes |= (elementType != val->getType());
    }
    vals.push_back(val);
  }

  if (differentTypes) {
    return llvm::ConstantStruct::getAnon(vals, true);
  }

  if (!elementType) {
    assert(ale->elements->dim == 0);
    elementType = DtoMemType(ale->type->toBasetype()->nextOf());
    return llvm::ConstantArray::get(LLArrayType::get(elementType, 0), vals);
  }

  llvm::ArrayType *t = llvm::ArrayType::get(elementType, ale->elements->dim);
  return llvm::ConstantArray::get(t, vals);
}

////////////////////////////////////////////////////////////////////////////////

void initializeArrayLiteral(IRState *p, ArrayLiteralExp *ale, LLValue *dstMem) {
  size_t elemCount = ale->elements->dim;

  // Don't try to write nothing to a zero-element array, we might represent it
  // as a null pointer.
  if (elemCount == 0)
    return;

  if (isConstLiteral(ale)) {
    llvm::Constant *constarr = arrayLiteralToConst(p, ale);

    // Emit a global for longer arrays, as an inline constant is always
    // lowered to a series of movs or similar at the asm level. The
    // optimizer can still decide to promote the memcpy intrinsic, so
    // the cutoff merely affects compilation speed.
    if (elemCount <= 4) {
      DtoStore(constarr, DtoBitCast(dstMem, getPtrToType(constarr->getType())));
    } else {
      auto gvar = new llvm::GlobalVariable(gIR->module, constarr->getType(),
                                           true, LLGlobalValue::InternalLinkage,
                                           constarr, ".arrayliteral");
#if LDC_LLVM_VER >= 309
      gvar->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
#else
      gvar->setUnnamedAddr(true);
#endif
      DtoMemCpy(dstMem, gvar,
                DtoConstSize_t(getTypeAllocSize(constarr->getType())));
    }
  } else {
    // Store the elements one by one.
    for (size_t i = 0; i < elemCount; ++i) {
      Expression *rhsExp = indexArrayLiteral(ale, i);

      LLValue *lhsPtr = DtoGEPi(dstMem, 0, i, "", p->scopebb());
      DLValue lhs(rhsExp->type, DtoBitCast(lhsPtr, DtoPtrToType(rhsExp->type)));

      // try to construct it in-place
      if (!toInPlaceConstruction(&lhs, rhsExp))
        DtoAssign(ale->loc, &lhs, toElem(rhsExp), TOKblit);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
LLConstant *DtoConstSlice(LLConstant *dim, LLConstant *ptr, Type *type) {
  LLConstant *values[2] = {dim, ptr};
  llvm::ArrayRef<LLConstant *> valuesRef = llvm::makeArrayRef(values, 2);
  LLStructType *lltype =
      type ? isaStruct(DtoType(type))
           : LLConstantStruct::getTypeForElements(gIR->context(), valuesRef);
  return LLConstantStruct::get(lltype, valuesRef);
}

////////////////////////////////////////////////////////////////////////////////

static DSliceValue *getSlice(Type *arrayType, LLValue *array) {
  LLType *llArrayType = DtoType(arrayType);
  if (array->getType() == llArrayType)
    return new DSliceValue(arrayType, array);

  LLValue *len = DtoExtractValue(array, 0, ".len");
  LLValue *ptr = DtoExtractValue(array, 1, ".ptr");
  ptr = DtoBitCast(ptr, llArrayType->getContainedType(1));

  return new DSliceValue(arrayType, len, ptr);
}

////////////////////////////////////////////////////////////////////////////////
DSliceValue *DtoNewDynArray(Loc &loc, Type *arrayType, DValue *dim,
                            bool defaultInit) {
  IF_LOG Logger::println("DtoNewDynArray : %s", arrayType->toChars());
  LOG_SCOPE;

  // typeinfo arg
  LLValue *arrayTypeInfo = DtoTypeInfoOf(arrayType);

  // dim arg
  assert(DtoType(dim->type) == DtoSize_t());
  LLValue *arrayLen = DtoRVal(dim);

  // get runtime function
  Type *eltType = arrayType->toBasetype()->nextOf();
  bool zeroInit = eltType->isZeroInit();

  const char *fnname = defaultInit
                           ? (zeroInit ? "_d_newarrayT" : "_d_newarrayiT")
                           : "_d_newarrayU";
  LLFunction *fn = getRuntimeFunction(loc, gIR->module, fnname);

  // call allocator
  LLValue *newArray =
      gIR->CreateCallOrInvoke(fn, arrayTypeInfo, arrayLen, ".gc_mem")
          .getInstruction();

  return getSlice(arrayType, newArray);
}

////////////////////////////////////////////////////////////////////////////////
DSliceValue *DtoNewMulDimDynArray(Loc &loc, Type *arrayType, DValue **dims,
                                  size_t ndims) {
  IF_LOG Logger::println("DtoNewMulDimDynArray : %s", arrayType->toChars());
  LOG_SCOPE;

  // typeinfo arg
  LLValue *arrayTypeInfo = DtoTypeInfoOf(arrayType);

  // get value type
  Type *vtype = arrayType->toBasetype();
  for (size_t i = 0; i < ndims; ++i) {
    vtype = vtype->nextOf();
  }

  // get runtime function
  const char *fnname =
      vtype->isZeroInit() ? "_d_newarraymTX" : "_d_newarraymiTX";
  LLFunction *fn = getRuntimeFunction(loc, gIR->module, fnname);

  // Check if constant
  bool allDimsConst = true;
  for (size_t i = 0; i < ndims; ++i) {
    if (!isaConstant(DtoRVal(dims[i]))) {
      allDimsConst = false;
    }
  }

  // build dims
  LLValue *array;
  if (allDimsConst) {
    // Build constant array for dimensions
    std::vector<LLConstant *> argsdims;
    argsdims.reserve(ndims);
    for (size_t i = 0; i < ndims; ++i) {
      argsdims.push_back(isaConstant(DtoRVal(dims[i])));
    }

    llvm::Constant *dims = llvm::ConstantArray::get(
        llvm::ArrayType::get(DtoSize_t(), ndims), argsdims);
    auto gvar = new llvm::GlobalVariable(gIR->module, dims->getType(), true,
                                         LLGlobalValue::InternalLinkage, dims,
                                         ".dimsarray");
    array = llvm::ConstantExpr::getBitCast(gvar, getPtrToType(dims->getType()));
  } else {
    // Build static array for dimensions
    LLArrayType *type = LLArrayType::get(DtoSize_t(), ndims);
    array = DtoRawAlloca(type, 0, ".dimarray");
    for (size_t i = 0; i < ndims; ++i) {
      DtoStore(DtoRVal(dims[i]), DtoGEPi(array, 0, i, ".ndim"));
    }
  }

  LLStructType *dtype = DtoArrayType(DtoSize_t());
  LLValue *darray = DtoRawAlloca(dtype, 0, ".array");
  DtoStore(DtoConstSize_t(ndims), DtoGEPi(darray, 0, 0, ".len"));
  DtoStore(DtoBitCast(array, getPtrToType(DtoSize_t())),
           DtoGEPi(darray, 0, 1, ".ptr"));

  // call allocator
  LLValue *newptr =
      gIR->CreateCallOrInvoke(fn, arrayTypeInfo, DtoLoad(darray), ".gc_mem")
          .getInstruction();

  IF_LOG Logger::cout() << "final ptr = " << *newptr << '\n';

  return getSlice(arrayType, newptr);
}

////////////////////////////////////////////////////////////////////////////////
DSliceValue *DtoResizeDynArray(Loc &loc, Type *arrayType, DValue *array,
                               LLValue *newdim) {
  IF_LOG Logger::println("DtoResizeDynArray : %s", arrayType->toChars());
  LOG_SCOPE;

  assert(array);
  assert(newdim);
  assert(arrayType);
  assert(arrayType->toBasetype()->ty == Tarray);

  // decide on what runtime function to call based on whether the type is zero
  // initialized
  bool zeroInit = arrayType->toBasetype()->nextOf()->isZeroInit();

  // call runtime
  LLFunction *fn =
      getRuntimeFunction(loc, gIR->module, zeroInit ? "_d_arraysetlengthT"
                                                    : "_d_arraysetlengthiT");

  LLValue *newArray =
      gIR->CreateCallOrInvoke(
             fn, DtoTypeInfoOf(arrayType), newdim,
             DtoBitCast(DtoLVal(array), fn->getFunctionType()->getParamType(2)),
             ".gc_mem")
          .getInstruction();

  return getSlice(arrayType, newArray);
}

////////////////////////////////////////////////////////////////////////////////

void DtoCatAssignElement(Loc &loc, Type *arrayType, DValue *array,
                         Expression *exp) {
  IF_LOG Logger::println("DtoCatAssignElement");
  LOG_SCOPE;

  assert(array);

  LLValue *oldLength = DtoArrayLen(array);

  // Do not move exp->toElem call after creating _d_arrayappendcTX,
  // otherwise a ~= a[$-i] won't work correctly
  DValue *expVal = toElem(exp);

  LLFunction *fn = getRuntimeFunction(loc, gIR->module, "_d_arrayappendcTX");
  LLValue *appendedArray =
      gIR->CreateCallOrInvoke(
             fn, DtoTypeInfoOf(arrayType),
             DtoBitCast(DtoLVal(array), fn->getFunctionType()->getParamType(1)),
             DtoConstSize_t(1), ".appendedArray")
          .getInstruction();
  appendedArray = DtoAggrPaint(appendedArray, DtoType(arrayType));

  LLValue *ptr = DtoArrayPtr(array);
  ptr = DtoGEP1(ptr, oldLength, true, ".lastElem");
  DLValue lastElem(arrayType->nextOf(), ptr);
  DtoAssign(loc, &lastElem, expVal, TOKblit);
  callPostblit(loc, exp, ptr);
}

////////////////////////////////////////////////////////////////////////////////

DSliceValue *DtoCatAssignArray(Loc &loc, DValue *arr, Expression *exp) {
  IF_LOG Logger::println("DtoCatAssignArray");
  LOG_SCOPE;
  Type *arrayType = arr->type;

  LLFunction *fn = getRuntimeFunction(loc, gIR->module, "_d_arrayappendT");
  // Call _d_arrayappendT(TypeInfo ti, byte[] *px, byte[] y)
  LLValue *newArray =
      gIR->CreateCallOrInvoke(
             fn, DtoTypeInfoOf(arrayType),
             DtoBitCast(DtoLVal(arr), fn->getFunctionType()->getParamType(1)),
             DtoAggrPaint(DtoSlice(exp),
                          fn->getFunctionType()->getParamType(2)),
             ".appendedArray")
          .getInstruction();

  return getSlice(arrayType, newArray);
}

////////////////////////////////////////////////////////////////////////////////

DSliceValue *DtoCatArrays(Loc &loc, Type *arrayType, Expression *exp1,
                          Expression *exp2) {
  IF_LOG Logger::println("DtoCatAssignArray");
  LOG_SCOPE;

  llvm::SmallVector<llvm::Value *, 3> args;
  LLFunction *fn = nullptr;

  if (exp1->op == TOKcat) { // handle multiple concat
    fn = getRuntimeFunction(loc, gIR->module, "_d_arraycatnTX");

    // Create array of slices
    typedef llvm::SmallVector<llvm::Value *, 16> ArgVector;
    ArgVector arrs;
    arrs.push_back(DtoSlicePtr(exp2));
    CatExp *ce = static_cast<CatExp *>(exp1);
    do {
      arrs.push_back(DtoSlicePtr(ce->e2));
      ce = static_cast<CatExp *>(ce->e1);
    } while (ce->op == TOKcat);
    arrs.push_back(DtoSlicePtr(ce));

    // Create static array from slices
    LLPointerType *ptrarraytype = isaPointer(arrs[0]->getType());
    assert(ptrarraytype && "Expected pointer type");
    LLStructType *arraytype = isaStruct(ptrarraytype->getElementType());
    assert(arraytype && "Expected struct type");
    LLArrayType *type = LLArrayType::get(arraytype, arrs.size());
    LLValue *array = DtoRawAlloca(type, 0, ".slicearray");
    unsigned int i = 0;
    for (ArgVector::reverse_iterator I = arrs.rbegin(), E = arrs.rend(); I != E;
         ++I) {
      LLValue *v = DtoLoad(DtoBitCast(*I, ptrarraytype));
      DtoStore(v, DtoGEPi(array, 0, i++, ".slice"));
    }

    LLStructType *type2 = DtoArrayType(arraytype);
    LLValue *array2 = DtoRawAlloca(type2, 0, ".array");
    DtoStore(DtoConstSize_t(arrs.size()), DtoGEPi(array2, 0, 0, ".len"));
    DtoStore(DtoBitCast(array, ptrarraytype), DtoGEPi(array2, 0, 1, ".ptr"));
    LLValue *val =
        DtoLoad(DtoBitCast(array2, getPtrToType(DtoArrayType(DtoArrayType(
                                       LLType::getInt8Ty(gIR->context()))))));

    // TypeInfo ti
    args.push_back(DtoTypeInfoOf(arrayType));
    // byte[][] arrs
    args.push_back(val);
  } else {
    fn = getRuntimeFunction(loc, gIR->module, "_d_arraycatT");

    // TypeInfo ti
    args.push_back(DtoTypeInfoOf(arrayType));
    // byte[] x
    LLValue *val = DtoLoad(DtoSlicePtr(exp1));
    val = DtoAggrPaint(val, fn->getFunctionType()->getParamType(1));
    args.push_back(val);
    // byte[] y
    val = DtoLoad(DtoSlicePtr(exp2));
    val = DtoAggrPaint(val, fn->getFunctionType()->getParamType(2));
    args.push_back(val);
  }

  auto newArray =
      gIR->funcGen().callOrInvoke(fn, args, ".appendedArray").getInstruction();
  return getSlice(arrayType, newArray);
}

////////////////////////////////////////////////////////////////////////////////

DSliceValue *DtoAppendDChar(Loc &loc, DValue *arr, Expression *exp,
                            const char *func) {
  LLValue *valueToAppend = DtoRVal(exp);

  // Prepare arguments
  LLFunction *fn = getRuntimeFunction(loc, gIR->module, func);

  // Call function (ref string x, dchar c)
  LLValue *newArray =
      gIR->CreateCallOrInvoke(
             fn,
             DtoBitCast(DtoLVal(arr), fn->getFunctionType()->getParamType(0)),
             DtoBitCast(valueToAppend, fn->getFunctionType()->getParamType(1)),
             ".appendedArray")
          .getInstruction();

  return getSlice(arr->type, newArray);
}

////////////////////////////////////////////////////////////////////////////////

DSliceValue *DtoAppendDCharToString(Loc &loc, DValue *arr, Expression *exp) {
  IF_LOG Logger::println("DtoAppendDCharToString");
  LOG_SCOPE;
  return DtoAppendDChar(loc, arr, exp, "_d_arrayappendcd");
}

////////////////////////////////////////////////////////////////////////////////

DSliceValue *DtoAppendDCharToUnicodeString(Loc &loc, DValue *arr,
                                           Expression *exp) {
  IF_LOG Logger::println("DtoAppendDCharToUnicodeString");
  LOG_SCOPE;
  return DtoAppendDChar(loc, arr, exp, "_d_arrayappendwd");
}

////////////////////////////////////////////////////////////////////////////////
// helper for eq and cmp
static LLValue *DtoArrayEqCmp_impl(Loc &loc, const char *func, DValue *l,
                                   DValue *r, bool useti) {
  IF_LOG Logger::println("comparing arrays");
  LLFunction *fn = getRuntimeFunction(loc, gIR->module, func);
  assert(fn);

  // find common dynamic array type
  Type *commonType = l->type->toBasetype()->nextOf()->arrayOf();

  // cast static arrays to dynamic ones, this turns them into DSliceValues
  Logger::println("casting to dynamic arrays");
  l = DtoCastArray(loc, l, commonType);
  r = DtoCastArray(loc, r, commonType);

  LLSmallVector<LLValue *, 3> args;

  // get values, reinterpret cast to void[]
  args.push_back(DtoAggrPaint(DtoRVal(l),
                              DtoArrayType(LLType::getInt8Ty(gIR->context()))));
  args.push_back(DtoAggrPaint(DtoRVal(r),
                              DtoArrayType(LLType::getInt8Ty(gIR->context()))));

  // pass array typeinfo ?
  if (useti) {
    LLValue *tival = DtoTypeInfoOf(l->type);
    args.push_back(DtoBitCast(tival, fn->getFunctionType()->getParamType(2)));
  }

  return gIR->funcGen().callOrInvoke(fn, args).getInstruction();
}

////////////////////////////////////////////////////////////////////////////////
LLValue *DtoArrayEquals(Loc &loc, TOK op, DValue *l, DValue *r) {
  LLValue *res = nullptr;

  // optimize comparisons against null by rewriting to `l.length op 0`
  if (r->isNull()) {
    const auto predicate = eqTokToICmpPred(op);
    res = gIR->ir->CreateICmp(predicate, DtoArrayLen(l), DtoConstSize_t(0));
  } else {
    res = DtoArrayEqCmp_impl(loc, "_adEq2", l, r, true);
    const auto predicate = eqTokToICmpPred(op, /* invert = */ true);
    res = gIR->ir->CreateICmp(predicate, res, DtoConstInt(0));
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
LLValue *DtoArrayCompare(Loc &loc, TOK op, DValue *l, DValue *r) {
  LLValue *res = nullptr;
  llvm::ICmpInst::Predicate cmpop;
  tokToICmpPred(op, false, &cmpop, &res);

  if (!res) {
    Type *t = l->type->toBasetype()->nextOf()->toBasetype();
    if (t->ty == Tchar) {
      res = DtoArrayEqCmp_impl(loc, "_adCmpChar", l, r, false);
    } else {
      res = DtoArrayEqCmp_impl(loc, "_adCmp2", l, r, true);
    }
    res = gIR->ir->CreateICmp(cmpop, res, DtoConstInt(0));
  }

  assert(res);
  return res;
}

////////////////////////////////////////////////////////////////////////////////
LLValue *DtoArrayCastLength(Loc &loc, LLValue *len, LLType *elemty,
                            LLType *newelemty) {
  IF_LOG Logger::println("DtoArrayCastLength");
  LOG_SCOPE;

  assert(len);
  assert(elemty);
  assert(newelemty);

  size_t esz = getTypeAllocSize(elemty);
  size_t nsz = getTypeAllocSize(newelemty);
  if (esz == nsz) {
    return len;
  }

  LLFunction *fn = getRuntimeFunction(loc, gIR->module, "_d_array_cast_len");
  return gIR
      ->CreateCallOrInvoke(fn, len, LLConstantInt::get(DtoSize_t(), esz, false),
                           LLConstantInt::get(DtoSize_t(), nsz, false))
      .getInstruction();
}

////////////////////////////////////////////////////////////////////////////////
LLValue *DtoDynArrayIs(TOK op, DValue *l, DValue *r) {
  assert(l);
  assert(r);

  LLValue *len1 = DtoArrayLen(l);
  LLValue *ptr1 = DtoArrayPtr(l);

  LLValue *len2 = DtoArrayLen(r);
  LLValue *ptr2 = DtoArrayPtr(r);

  return createIPairCmp(op, len1, ptr1, len2, ptr2);
}

////////////////////////////////////////////////////////////////////////////////
LLValue *DtoArrayLen(DValue *v) {
  IF_LOG Logger::println("DtoArrayLen");
  LOG_SCOPE;

  Type *t = v->type->toBasetype();
  if (t->ty == Tarray) {
    if (v->isNull()) {
      return DtoConstSize_t(0);
    }
    if (v->isLVal()) {
      return DtoLoad(DtoGEPi(DtoLVal(v), 0, 0), ".len");
    }
    return gIR->ir->CreateExtractValue(DtoRVal(v), 0, ".len");
  }
  if (t->ty == Tsarray) {
    assert(!v->isSlice());
    assert(!v->isNull());
    TypeSArray *sarray = static_cast<TypeSArray *>(t);
    return DtoConstSize_t(sarray->dim->toUInteger());
  }
  llvm_unreachable("unsupported array for len");
}

////////////////////////////////////////////////////////////////////////////////
LLValue *DtoArrayPtr(DValue *v) {
  IF_LOG Logger::println("DtoArrayPtr");
  LOG_SCOPE;

  Type *t = v->type->toBasetype();
  // v's LL array element type may not be the real one
  // due to implicit casts (e.g., to base class)
  LLType *wantedLLPtrType = DtoPtrToType(t->nextOf());
  LLValue *ptr = nullptr;

  if (t->ty == Tarray) {
    if (v->isNull()) {
      ptr = getNullPtr(wantedLLPtrType);
    } else if (v->isLVal()) {
      ptr = DtoLoad(DtoGEPi(DtoLVal(v), 0, 1), ".ptr");
    } else {
      ptr = gIR->ir->CreateExtractValue(DtoRVal(v), 1, ".ptr");
    }
  } else if (t->ty == Tsarray) {
    assert(!v->isSlice());
    assert(!v->isNull());
    ptr = DtoLVal(v);
  } else {
    llvm_unreachable("Unexpected array type.");
  }

  return DtoBitCast(ptr, wantedLLPtrType);
}

////////////////////////////////////////////////////////////////////////////////
DValue *DtoCastArray(Loc &loc, DValue *u, Type *to) {
  IF_LOG Logger::println("DtoCastArray");
  LOG_SCOPE;

  LLType *tolltype = DtoType(to);

  Type *totype = to->toBasetype();
  Type *fromtype = u->type->toBasetype();
  if (fromtype->ty != Tarray && fromtype->ty != Tsarray) {
    error(loc, "can't cast %s to %s", u->type->toChars(), to->toChars());
    fatal();
  }

  IF_LOG Logger::cout() << "from array or sarray" << '\n';

  if (totype->ty == Tpointer) {
    IF_LOG Logger::cout() << "to pointer" << '\n';
    LLValue *ptr = DtoArrayPtr(u);
    if (ptr->getType() != tolltype) {
      ptr = gIR->ir->CreateBitCast(ptr, tolltype);
    }
    return new DImValue(to, ptr);
  }

  if (totype->ty == Tarray) {
    IF_LOG Logger::cout() << "to array" << '\n';

    LLValue *length = nullptr;
    LLValue *ptr = nullptr;
    if (fromtype->ty == Tsarray) {
      uinteger_t len = static_cast<TypeSArray *>(fromtype)->dim->toUInteger();
      length = DtoConstSize_t(len);
      ptr = DtoLVal(u);
      assert(isaPointer(ptr->getType()));
      LLArrayType *arrty = isaArray(ptr->getType()->getContainedType(0));

      if (arrty->getNumElements() * fromtype->nextOf()->size() %
              totype->nextOf()->size() !=
          0) {
        error(loc,
              "invalid cast from '%s' to '%s', the element sizes don't line up",
              fromtype->toChars(), totype->toChars());
        fatal();
      }
    } else {
      length = DtoArrayLen(u);
      ptr = DtoArrayPtr(u);
    }

    LLType *ptrty = DtoArrayType(totype)->getContainedType(1);
    LLType *ety = DtoMemType(fromtype->nextOf());

    if (fromtype->nextOf()->size() != totype->nextOf()->size())
      length = DtoArrayCastLength(loc, length, ety, ptrty->getContainedType(0));

    return new DSliceValue(to, length, DtoBitCast(ptr, ptrty));
  }

  if (totype->ty == Tsarray) {
    IF_LOG Logger::cout() << "to sarray" << '\n';

    LLValue *ptr = nullptr;
    if (fromtype->ty == Tsarray) {
      ptr = DtoLVal(u);
    } else {
      size_t tosize = static_cast<TypeSArray *>(totype)->dim->toInteger();
      size_t i =
          (tosize * totype->nextOf()->size() - 1) / fromtype->nextOf()->size();
      DConstValue index(Type::tsize_t, DtoConstSize_t(i));
      DtoIndexBoundsCheck(loc, u, &index);
      ptr = DtoArrayPtr(u);
    }

    return new DLValue(to, DtoBitCast(ptr, getPtrToType(tolltype)));
  }

  if (totype->ty == Tbool) {
    // return (arr.ptr !is null)
    LLValue *ptr = DtoArrayPtr(u);
    LLConstant *nul = getNullPtr(ptr->getType());
    return new DImValue(to, gIR->ir->CreateICmpNE(ptr, nul));
  }

  const auto castedPtr = DtoBitCast(DtoArrayPtr(u), getPtrToType(tolltype));
  return new DLValue(to, castedPtr);
}

void DtoIndexBoundsCheck(Loc &loc, DValue *arr, DValue *index) {
  Type *arrty = arr->type->toBasetype();
  assert(
      (arrty->ty == Tsarray || arrty->ty == Tarray || arrty->ty == Tpointer) &&
      "Can only array bounds check for static or dynamic arrays");

  if (!index) {
    // Caller supplied no index, known in-bounds.
    return;
  }

  if (arrty->ty == Tpointer) {
    // Length of pointers is unknown, ingore.
    return;
  }

  llvm::ICmpInst::Predicate cmpop = llvm::ICmpInst::ICMP_ULT;
  llvm::Value *cond = gIR->ir->CreateICmp(cmpop, DtoRVal(index),
                                          DtoArrayLen(arr), "bounds.cmp");

  llvm::BasicBlock *okbb = gIR->insertBB("bounds.ok");
  llvm::BasicBlock *failbb = gIR->insertBBAfter(okbb, "bounds.fail");
  gIR->ir->CreateCondBr(cond, okbb, failbb);

  // set up failbb to call the array bounds error runtime function
  gIR->scope() = IRScope(failbb);
  DtoBoundsCheckFailCall(gIR, loc);

  // if ok, proceed in okbb
  gIR->scope() = IRScope(okbb);
}

void DtoBoundsCheckFailCall(IRState *irs, Loc &loc) {
  llvm::Function *errorfn =
      getRuntimeFunction(loc, irs->module, "_d_arraybounds");
  irs->CreateCallOrInvoke(
      errorfn, DtoModuleFileName(irs->func()->decl->getModule(), loc),
      DtoConstUint(loc.linnum));

  // the function does not return
  irs->ir->CreateUnreachable();
}
