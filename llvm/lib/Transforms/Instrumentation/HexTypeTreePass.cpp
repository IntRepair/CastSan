//===-- HexTypeTreePass.cpp -----------------------------------------------===//
//
// This file is a part of HexType, a type confusion detector.
//
// The HexTypeTreePass has below two functions:
//   - Create object relationship information.
//   - Compile time verification
// This pass will run before all optimization passes run
// The rest is handled by the run-time library.
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/HexTypeUtil.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Support/raw_ostream.h"

#include <cxxabi.h>
#include <iostream>

#define MAXLEN 10000

using namespace llvm;

namespace {
  struct HexTypeTree : public ModulePass {
    static char ID;
    HexTypeTree() : ModulePass(ID) {}

    TargetLibraryInfo *tli;
    TargetLibraryInfoImpl tlii;

    HexTypeLLVMUtil *HexTypeUtilSet;

	void emitObjTypeMapInit(Module &M) {
      FunctionType *FTy = FunctionType::get(HexTypeUtilSet->VoidTy, false);
      Function *F = Function::Create(FTy, GlobalValue::InternalLinkage,
                                     "__init", &M);

      F->setUnnamedAddr(true);
      F->setLinkage(GlobalValue::InternalLinkage);
      F->addFnAttr(Attribute::NoInline);

      BasicBlock *BB = BasicBlock::Create(M.getContext(), "entry", F);
      IRBuilder<> Builder(BB);

      std::string initName = "__init_obj_type_map";
      //Paul: get the ypdate phantom info function or insert it
      Constant *GCOVInit = M.getOrInsertFunction(initName,
                                                 HexTypeUtilSet->VoidTy,
                                                 nullptr);
      //Paul: create a call 
      Builder.CreateCall(GCOVInit);
      //Paul: the above call returns void
      Builder.CreateRetVoid();
      //Paul: ?
      appendToGlobalCtors(M, F, 0);
    }


     //Paul: instrument IR with object tracing functions
    void emitExtendObjTraceInst(Module &M, int hashIndex,
                                CallInst *call, int extendTarget) {
      ConstantInt *HashValueConst =
        dyn_cast<ConstantInt>(call->getOperand(1));
      uint64_t TargetHashValue = HashValueConst->getZExtValue();

      if (ClCastObjOpt) {
        bool isSafeObj = true;
        std::set<std::string>::iterator it;
        for (it = HexTypeUtilSet->CastingRelatedSet.begin();
             it != HexTypeUtilSet->CastingRelatedSet.end(); ++it) {
          std::string TargetStr = *it;
          uint64_t TmpHash = HexTypeUtilSet->getHashValueFromStr(TargetStr);
          if (TmpHash == TargetHashValue)
            isSafeObj = false;
        }

        if (isSafeObj) return;
      }

      std::string funName;
      //Paul: for the new operator
      if (extendTarget == PLACEMENTNEW)
        funName.assign("__placement_new_handle");
      else if (extendTarget == UPCAST)
	    funName.assign("__poly_upcasting_handle");
      else
        //Paul: for the reinterpret cast 
        funName.assign("__reinterpret_casting_handle");

      Instruction *next = HexTypeUtilSet->findNextInstruction(call);
      IRBuilder<> Builder(next);

      StructElementInfoTy Elements;
      StructType * LLVMType = nullptr;
      if (extendTarget == UPCAST)
      {
	      bool found = false;
	      for (auto & type : HexTypeUtilSet->AllTypeInfo)
		      if (type.DetailInfo.TypeHashValue == TargetHashValue)
		      {
			      HexTypeUtilSet->getArrayOffsets(type.StructTy, Elements, 0);
			      assert (Elements.size() && "No Elements?");
			      LLVMType = type.StructTy;
			      found = true;
			      break;
		      }

	      assert (found && "Type not found?");
      }
      else
      {
	      StructType *EmptySet = nullptr;
	      Elements.push_back(std::make_pair(TargetHashValue, EmptySet));
      }
      

      Value *first = Builder.CreatePtrToInt(call->getOperand(0),
                                            HexTypeUtilSet->IntptrTyN);
      Value *second = Builder.CreatePtrToInt(call->getOperand(2),
                                             HexTypeUtilSet->IntptrTyN);
      Value *NewAddr = Builder.CreateAdd(first, second);
      Value *ObjAddrT = Builder.CreateIntToPtr(NewAddr,
                                               HexTypeUtilSet->IntptrTyN);
      HexTypeUtilSet->insertUpdate(&M, Builder, funName, ObjAddrT,
                                   Elements, 0, NULL, NULL, NULL, LLVMType);
      if (ClMakeLogInfo) {
        Function *ObjUpdateFunction =
            //Paul: insert the object update count function
            (Function*)M.getOrInsertFunction(
            "__obj_update_count", HexTypeUtilSet->VoidTy,
            HexTypeUtilSet->Int32Ty,
            HexTypeUtilSet->Int64Ty,
            nullptr);
        Value *AllocType;
        if (extendTarget == PLACEMENTNEW)
          AllocType = ConstantInt::get(HexTypeUtilSet->Int32Ty, PLACEMENTNEW);
        else
          AllocType = ConstantInt::get(HexTypeUtilSet->Int32Ty, REINTERPRET);
        Value *TmpOne = ConstantInt::get(HexTypeUtilSet->Int64Ty, 1);
        Value *Param[2] = {AllocType, TmpOne};
        //Paul: create the the call to obj update function with params
        Builder.CreateCall(ObjUpdateFunction, Param);
      }
    }
    
    //Paul: 
    void extendClangInstrumentation(Module &M) {
      for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F)
        for (Function::iterator BB = F->begin(), E = F->end(); BB != E;) {
          bool isUpdated = false;
          for (BasicBlock::iterator i = BB->begin(), ie = BB->end();
               i != ie; ++i)
            if (CallInst *call = dyn_cast<CallInst>(&*i))
              if (call->getCalledFunction() != nullptr) {
                std::string FnName = call->getCalledFunction()->getName();
                if (FnName.compare("__placement_new_handle") == 0 ||
                    FnName.compare("__reinterpret_casting_handle") == 0 ||
                    FnName.compare("__poly_upcasting_handle") == 0) {
                  if (HexTypeUtilSet->AllTypeInfo.size() > 0) {
                    if (FnName.compare("__placement_new_handle") == 0)
                      //Paul: trace the new operator
                      emitExtendObjTraceInst(M, 1, call, PLACEMENTNEW);
                    else if (FnName.compare("__reinterpret_casting_handle") == 0)
                      //Paul: trace the reinterpret instruction
                      emitExtendObjTraceInst(M, 1, call, REINTERPRET);
                    else if (FnName.compare("__poly_upcasting_handle") == 0)
                      emitExtendObjTraceInst(M, 1, call, UPCAST);
                  }
                  (&*i)->eraseFromParent();
                  isUpdated = true;
                  break;
                }
              }
          if(isUpdated == false)
            BB++;
        }
    }
   
    //Paul: check if it is alloca call
    bool isAllocCall(CallInst *val) {
      if (isAllocationFn(val, this->tli) &&
          (isMallocLikeFn(val, this->tli) || isCallocLikeFn(val, this->tli) ||
           !isAllocLikeFn(val, this->tli)))
        return true;
      return false;
    }
    
    //Paul: collect heap allocations
    void collectHeapAlloc(CallInst *call,
                        std::map<CallInst *, Type *> *heapObjsNew) {
      std::string functionName;
      if (call->getCalledFunction() != nullptr)
        functionName.assign(call->getCalledFunction()->getName());

      int unmangledStatus;
      char *unmangledName =
        abi::__cxa_demangle(functionName.c_str(), nullptr,
                            nullptr, &unmangledStatus);
      bool isOverloadedNew = false;
      if (unmangledStatus == 0) {
        std::string unmangledNameStr(unmangledName);
        if (unmangledNameStr.find("::operator new(unsigned long)") !=
            std::string::npos ||
            unmangledNameStr.find(
              "::operator new(unsigned long, std::nothrow_t const&)") !=
            std::string::npos ||
            unmangledNameStr.find("::operator new[](unsigned long)") !=
            std::string::npos ||
            unmangledNameStr.find(
              "::operator new[](unsigned long, std::nothrow_t const&)")
            != std::string::npos) {
          isOverloadedNew = true;
        }
      }

      if (isAllocCall(call) || isOverloadedNew)
        if (Type *allocTy = getMallocAllocatedType(call, this->tli))
          if (HexTypeUtilSet->isInterestingType(allocTy))
            heapObjsNew->insert(
              std::pair<CallInst *, Type *>(call, allocTy));

      return;
    }
    
    //Paul: collect free usages
    void collectFree(CallInst *call, Instruction *InstPrev,
                   std::map<CallInst *, Type *> *heapObjsFree) {
      if (isFreeCall(call, this->tli))
        if (const BitCastInst *BCI = dyn_cast<BitCastInst>(InstPrev))
          if (PointerType *FreeType =
              cast<PointerType>(BCI->getSrcTy())) {
            Type *VTy = FreeType->getElementType();
            if (HexTypeUtilSet->isInterestingType(VTy))
                heapObjsFree->insert(
                  std::pair<CallInst *, Type *>(call, VTy));
          }

      return;
    }
    
    //Paul: check if it is a realloc function
    bool isReallocFn(Function *F) {
      std::string funName = F->getName().str();
      if ((funName.find("realloc") != std::string::npos))
        return true;

      return false;
    }
    
    //Paul: handle the heap alloc 
    void handleHeapAlloc(Module &M, std::map<CallInst *, Type *> *heapObjsNew) {
      for (std::map<CallInst *, Type *>::iterator it=heapObjsNew->begin();
           it!=heapObjsNew->end(); ++it) {

        Instruction *next = HexTypeUtilSet->findNextInstruction(it->first);
        IRBuilder<> Builder(next);

        bool isRealloc = 0;

        StructElementInfoTy offsets;
        HexTypeUtilSet->getArrayOffsets(it->second, offsets, 0);
        if (offsets.size() == 0) continue;

        Value *ArraySize;
        Value *TypeSize;
        Value *ArraySizeF = nullptr;
        if (isMallocLikeFn(it->first, this->tli) ||
            !isAllocLikeFn(it->first, this->tli) ||
            !isAllocationFn(it->first, this->tli)) {
          if (isMallocLikeFn(it->first, this->tli) ||
              !isAllocationFn(it->first, this->tli))
            ArraySize = it->first->getArgOperand(0);
          else
            ArraySize = it->first->getArgOperand(1);

          unsigned long TypeSizeVal =
            HexTypeUtilSet->DL.getTypeAllocSize(it->second);
          TypeSize = ConstantInt::get(HexTypeUtilSet->Int64Ty, TypeSizeVal);

          if (TypeSizeVal != 0)
            ArraySizeF = Builder.CreateUDiv(ArraySize, TypeSize);
          else
            ArraySizeF = ArraySize;

          if (isReallocFn(it->first->getCalledFunction()))
            isRealloc = true;
        }

        else if (isCallocLikeFn(it->first, this->tli))
          ArraySizeF = it->first->getArgOperand(1);

        if (ArraySizeF) {
          if (isRealloc == 1)
            //Paul: this functions will call into the runtime-rt to make necessary
            //object information updates
            HexTypeUtilSet->insertUpdate(&M, Builder, "__update_realloc_oinfo",
                                         (Value *)(it->first), offsets,
                                         HexTypeUtilSet->DL.getTypeAllocSize(
                                           it->second), ArraySizeF,
                                         (Value *)(it->first->getArgOperand(0)),
                                         NULL, it->second);

          else
            //Paul: the same as above but for heap allocations
            HexTypeUtilSet->insertUpdate(&M, Builder, "__update_heap_oinfo",
                                         (Value *)(it->first), offsets,
                                         HexTypeUtilSet->DL.getTypeAllocSize(
                                           it->second), ArraySizeF,
                                         NULL, NULL, it->second);
        }
      }
    }
    
    //Paul: in case of a free operation it will be called in the compiler-rt the
    //remove heap object info
    void handleFree(Module &M, std::map<CallInst *, Type *> *heapObjsFree) {
      for (std::map<CallInst *, Type *>::iterator it=heapObjsFree->begin();
           it!=heapObjsFree->end(); ++it) {
        Instruction *next = HexTypeUtilSet->findNextInstruction(it->first);
        IRBuilder<> Builder(next);
        StructElementInfoTy offsets;
        HexTypeUtilSet->getArrayOffsets(it->second, offsets, 0);
        HexTypeUtilSet->insertRemove(&M, Builder, "__remove_heap_oinfo",
                                     it->first->getArgOperand(0), offsets,
                                     0, HexTypeUtilSet->DL.getTypeAllocSize(
                                       it->second),
                                     NULL);
      }
    }
    
    //Paul: heap object tracing
    void heapObjTracing(Module &M) {
      Instruction *InstPrev;
      this->tli = new TargetLibraryInfo(tlii);
      std::map<CallInst *, Type *> heapObjsFree, heapObjsNew;
      for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F)
        for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB) {
          for (BasicBlock::iterator i = BB->begin(),
               ie = BB->end(); i != ie; ++i) {
            if (CallInst *call = dyn_cast<CallInst>(&*i)) {
              collectHeapAlloc(call, &heapObjsNew);
              collectFree(call, InstPrev, &heapObjsFree);
            }
            InstPrev = &*i;
          }

          handleHeapAlloc(M, &heapObjsNew);
          handleFree(M, &heapObjsFree);
          
          //Paul: heap and new object removal
          heapObjsFree.clear();
          heapObjsNew.clear();
        }
    }

    bool isLocalPointer(Value *target, Module::iterator F) {
      for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB)
        for (BasicBlock::iterator t = BB->begin(), te = BB->end();
             t != te; ++t)
          if (target == dyn_cast<AllocaInst>(&*t))
            return true;

      return false;
    }

    bool isSafePointer(Value *target, Module::iterator F, Module *M) {
      if (!isLocalPointer(target, F))
        return false;

      for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB)
        for (BasicBlock::iterator t = BB->begin(), te = BB->end();
             t != te; ++t)
          if (StoreInst *AI = dyn_cast<StoreInst>(&*t))
            if (target == AI->getPointerOperand())
              if (!isSafeSrcValue(AI->getValueOperand(), F, M))
                return false;

      return true;
    }
    
    //Paul: check if is an heap object
    bool isHeapObj(CallInst *call) {
      bool isOverloadedNew = false;
      std::string functionName = "";
      if (call->getCalledFunction() != nullptr)
        functionName = call->getCalledFunction()->getName();

      this->tli = new TargetLibraryInfo(tlii);
      int unmangledStatus;
      char *unmangledName =
        abi::__cxa_demangle(functionName.c_str(), nullptr,
                            nullptr, &unmangledStatus);
      if (unmangledStatus == 0) {
        std::string unmangledNameStr(unmangledName);
        if (unmangledNameStr.find("::operator new(unsigned long)") !=
            std::string::npos ||
            unmangledNameStr.find(
              "::operator new(unsigned long, std::nothrow_t const&)") !=
            std::string::npos ||
            unmangledNameStr.find("::operator new[](unsigned long)") !=
            std::string::npos ||
            unmangledNameStr.find(
              "::operator new[](unsigned long, std::nothrow_t const&)")
            != std::string::npos) {
          isOverloadedNew = true;
        }
      }

      if (isAllocCall(call) || isOverloadedNew)
        if (Type *allocTy = getMallocAllocatedType(call, this->tli))
          if (HexTypeUtilSet->isInterestingType(allocTy))
            return true;

      return false;
    }
    
    //Paul: ?
    bool isSafeSrcValue(Value *SrcValue, Module::iterator F,
                     Module *M) {
      // source is global variable
      for (llvm::Module::global_iterator ii = M->global_begin();
           ii != M->global_end(); ++ii) {
        if (&*ii == SrcValue)
          return true;
      }

      for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB)
        for (BasicBlock::iterator t = BB->begin(), te = BB->end();
             t != te; ++t) {
          if (SrcValue == (&*t)) {
            // source type is local stack value
            if (AllocaInst *AI = dyn_cast<AllocaInst>(&*t)) {
              if (HexTypeUtilSet->isInterestingType(AI->getAllocatedType()) &&
                  isa<StructType>(AI->getAllocatedType()))
                return true;

              else if(AI->getAllocatedType()->isPointerTy())
                if(isSafePointer(AI, F, M))
                  return true;
              return false;
            }

            // source type is heap object
            CallInst *call = dyn_cast<CallInst>(&*t);
            if (call) {
              if (isHeapObj(call))
                return true;
              return false;
            }

            // if source type is array (not pointer)
            if (GEPOperator *GO = dyn_cast<GEPOperator>(&*t))
              if (isa<StructType>(GO->getResultElementType()))
                return true;

            // if source type is pointer
            if (LoadInst *AI = dyn_cast<LoadInst>(&*t))
              if (isSafePointer(AI->getPointerOperand(), F, M))
                return true;

            // check store instruction
            if (StoreInst *AI = dyn_cast<StoreInst>(&*t))
              if (isSafeSrcValue(AI->getValueOperand(), F, M))
                return true;

            // if source type is related to bitcast
            if (const BitCastInst *BCI = dyn_cast<BitCastInst>(&*t))
              if (isSafeSrcValue(BCI->getOperand(0), F, M))
                return true;
          }
        }

      return false;
    }
    
    //Paul: see above function
    bool compiletime_verification(Value *SrcValue, Module::iterator F,
                                  Module *M) {
      if (isSafeSrcValue(SrcValue, F, M)) {
        // PointerType *FreeType = nullptr;
        // Type *DstTy= nullptr;
        // compile_time_verification(src, dst);
        return true;
      }

      return false;
    }
    
    //Paul: remvoe the basic blocks which are calling the type cast verification
    //functions.
    void compileTimeVerification(Module &M) {
      for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F)
        for (Function::iterator BB = F->begin(), E = F->end(); BB != E;) {
          bool isRemoved = false;
          for (BasicBlock::iterator i = BB->begin(),
               ie = BB->end(); i != ie; ++i)
            if (CallInst *call = dyn_cast<CallInst>(&*i))
              if (call->getCalledFunction() != nullptr) {
                std::string functionName = call->getCalledFunction()->getName();
                if ((functionName.compare("__type_casting_verification") == 0 ||
                     functionName.compare("__type_casting_verification_changing") == 0))
                  if (PtrToIntInst *SrcValue =
                      dyn_cast<PtrToIntInst>(call->getArgOperand(0)))
                      //Paul: ?
                      if (compiletime_verification(SrcValue->getPointerOperand(),
                                                 F, &M)) {
                      (&*i)->eraseFromParent();
                      isRemoved = true;
                      break;
                    }
              }

          if(isRemoved == false)
            BB++;
        }
    }

	  void typecastinginsertranges(Module &M) {
		  for (Module::iterator F = M.begin(), E = M.end(); F != E; F++)
		  {
			  for (Function::iterator BB = F->begin(), EB = F->end(); BB != EB;)
			  {
				  for (BasicBlock::iterator I = BB->begin(), EI = BB->end(); I != EI; I++)
				  {
					  if (CallInst * Call = dyn_cast<CallInst>(I))
					  {
						  if (auto Func = Call->getCalledFunction())
						  {
							  if (Func->getName().compare("__type_casting_verification") == 0 ||
							      Func->getName().compare("__type_casting_verification_changing") == 0)
							  {
								  int o = 0;
								  if (Func->getName().compare("__type_casting_verification_changing") == 0)
									  o = 1;
								  ConstantInt * ConstCastTypeHash = dyn_cast<ConstantInt>(Call->getArgOperand(1 + o));
								  ConstantInt * ConstPointerTypeHash = dyn_cast<ConstantInt>(Call->getArgOperand(2 + o));

								  uint64_t CastTypeH = ConstCastTypeHash->getZExtValue();
								  uint64_t PointerTypeH = ConstPointerTypeHash->getZExtValue();
								  
								  CastSanUtil & CastSan = HexTypeUtilSet->CastSan;								  

								  CHTreeNode & CastType = CastSan.Types[CastTypeH];
								  CHTreeNode & PointerType = CastSan.Types[PointerTypeH];

								  assert (CastType.TypeHash == CastTypeH && PointerType.TypeHash == PointerTypeH && "Type not found in CastSan MD!");

								  CHTreeNode * RootForCast = CastSan.getRootForCast(&PointerType, &CastType);
								  assert (RootForCast && "CastSan thinks this cast should be illegal");

								  Constant * ConstRangeStart = nullptr;
								  Constant * ConstRangeWidth = nullptr;
								  for (CHTreeNode::TreeIndex & Index : CastType.TreeIndices)
								  {
									  if (Index.first == RootForCast)
									  {
										  uint64_t RangeStart = Index.second;
										  uint64_t RangeWidth = CastSan.getRangeWidth(&CastType, Index.first);
										  
										  ConstRangeStart = ConstantInt::get(HexTypeUtilSet->Int64Ty, RangeStart);
										  ConstRangeWidth = ConstantInt::get(HexTypeUtilSet->Int64Ty, RangeWidth);
										  break;
									  }
								  }

								  assert(ConstRangeStart && ConstRangeWidth && "TreeIndex is suddenly missing");

								  Call->setArgOperand(2 + o, ConstRangeStart);
								  Call->setArgOperand(3 + o, ConstRangeWidth);

							  }
						  }
					  }
				  }
				  BB++;
			  }
		  }
	  }
    
    //Paul: generic function for starting the module
    virtual bool runOnModule(Module &M) {
      // Init HexTypeUtil
      HexTypeLLVMUtil HexTypeUtilSetT(M.getDataLayout());
      HexTypeUtilSet = &HexTypeUtilSetT;
      HexTypeUtilSet->initType(M);

      emitObjTypeMapInit(M);

      // Create type releationship information
      //Paul: todo
      HexTypeUtilSet->createObjRelationInfo(M);

      // Init for only tracing casting related objects
      //Paul: todo
      if (ClCastObjOpt || ClCreateCastRelatedTypeList)
        HexTypeUtilSet->setCastingRelatedSet();
      if (ClCreateCastRelatedTypeList)
        HexTypeUtilSet->extendCastingRelatedTypeSet();

      typecastinginsertranges(M);

      // Apply compile time verfication optimization
      //Paul: todo
      if (ClCompileTimeVerifyOpt)
        compileTimeVerification(M);

      // Heap object trace
      //Paul: todo
      heapObjTracing(M);

      // Extend HexType's clang Instrumentation
      //Paul: todo
      extendClangInstrumentation(M);

      return false;
    }
  };
}

//register pass
char HexTypeTree::ID = 0;

INITIALIZE_PASS(HexTypeTree, "HexTypeTree",
                "HexTypePass: fast type safety for C++ programs.",
                false, false)

ModulePass *llvm::createHexTypeTreePass() {
  return new HexTypeTree();
}
