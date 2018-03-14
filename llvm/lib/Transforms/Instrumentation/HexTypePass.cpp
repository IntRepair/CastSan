//===-- HexTypePass.cpp -----------------------------------------------===//
//
// This file is a part of HexType, a type confusion detector.
//
// The HexTypePass has below two functions:
//   - Track Stack object allocation
//   - Track Global object allocation
// This pass will run after all optimization passes run
// The rest is handled by the run-time library.
//===------------------------------------------------------------------===//

#include "llvm/Analysis/CallGraph.h"
#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/HexTypeUtil.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Instrumentation.h"

#include <iostream>

using namespace llvm;
#define MAXLEN 10000

namespace {

  struct HexType : public ModulePass {
    static char ID;
    HexType() : ModulePass(ID) {}

    HexTypeLLVMUtil *HexTypeUtilSet;
    CallGraph *CG;
    
    //Paul: store alloca set info.
    std::list<AllocaInst *> AllAllocaSet;
    //Paul: store lifetime end set, most likely to know when it is safe to delete obj from
    //object type map
    std::map<AllocaInst *, IntrinsicInst *> LifeTimeEndSet;
    //Paul: as above but this is the start set.
    std::map<AllocaInst *, IntrinsicInst *> LifeTimeStartSet;
    //Paul: return instruction set
    std::map<Function *, std::vector<Instruction *> *> ReturnInstSet;
    //Paul: all allocations with function set
    std::map<Instruction *, Function *> AllAllocaWithFnSet;
    //Paul: cast map
    std::map<Function*, bool> mayCastMap;

    void getAnalysisUsage(AnalysisUsage &Info) const {
      Info.addRequired<CallGraphWrapperPass>();
    }

    Function *setGlobalObjUpdateFn(Module &M) {
      FunctionType *VoidFTy =
        FunctionType::get(Type::getVoidTy(M.getContext()), false);
      Function *FGlobal = Function::Create(VoidFTy,
                                           GlobalValue::InternalLinkage,
                                           "__init_global_object", &M);
      FGlobal->setUnnamedAddr(true);
      FGlobal->setLinkage(GlobalValue::InternalLinkage);
      FGlobal->addFnAttr(Attribute::NoInline);

      return FGlobal;
    }
    
    //Paul: handling of function parameters
    void handleFnPrameter(Module &M, Function *F) {
      if (F->empty() || F->getEntryBlock().empty() ||
          F->getName().startswith("__init_global_object"))
        return;

      Type *MemcpyParams[] = { HexTypeUtilSet->Int8PtrTy,
        HexTypeUtilSet->Int8PtrTy,
        HexTypeUtilSet->Int64Ty };
      Function *MemcpyFunc =
        Intrinsic::getDeclaration(&M, Intrinsic::memcpy, MemcpyParams);
       
      for (auto &a : F->args()) {
        Argument *Arg = dyn_cast<Argument>(&a);
        if (!Arg->hasByValAttr())
          return;
        Type *ArgPointedTy = Arg->getType()->getPointerElementType();
        //Paul: is interesting if it is a struct or an array
        if (HexTypeUtilSet->isInterestingType(ArgPointedTy)) {
          //Paul: determine the size of the current argument
          unsigned long size =
            HexTypeUtilSet->DL.getTypeStoreSize(ArgPointedTy);
          IRBuilder<> B(&*(F->getEntryBlock().getFirstInsertionPt()));
          //Paul: create new allocation
          Value *NewAlloca = B.CreateAlloca(ArgPointedTy);
          //Paul: replace the uses of this argument with the new allocation
          Arg->replaceAllUsesWith(NewAlloca);
          //Paul: source address
          Value *Src = B.CreatePointerCast(Arg,
                                           HexTypeUtilSet->Int8PtrTy);
          //Paul: destination address
          Value *Dst = B.CreatePointerCast(NewAlloca,
                                           HexTypeUtilSet->Int8PtrTy);
          //Paul: destination and source addresses are added to the new param. list
          Value *Param[5] = { Dst, Src,
            ConstantInt::get(HexTypeUtilSet->Int64Ty, size),
            ConstantInt::get(HexTypeUtilSet->Int32Ty, 1),
            ConstantInt::get(HexTypeUtilSet->Int1Ty, 0) };
          //Paul: a new memcpy call with new parameters.
          B.CreateCall(MemcpyFunc, Param);
        }
      }
    }
    
    //Paul: determine the start and end time for an instruction and store it
    void collectLifeTimeInfo(Instruction *I) {
      if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(I))
        if ((II->getIntrinsicID() == Intrinsic::lifetime_start)
           || (II->getIntrinsicID() == Intrinsic::lifetime_end)) {
          ConstantInt *Size =
            dyn_cast<ConstantInt>(II->getArgOperand(0));
          if (Size->isMinusOne()) return;

          if (AllocaInst *AI =
             HexTypeUtilSet->findAllocaForValue(II->getArgOperand(1))) {
            if (II->getIntrinsicID() == Intrinsic::lifetime_start)
              LifeTimeStartSet.insert(std::pair<AllocaInst *,
                                   IntrinsicInst *>(AI, II));

            else if (II->getIntrinsicID() == Intrinsic::lifetime_end)
              LifeTimeEndSet.insert(std::pair<AllocaInst *,
                                 IntrinsicInst *>(AI, II));
          }
        }
    }
    
    //Paul: collect alloca info.
    void collectAllocaInstInfo(Instruction *I) {
      //Paul: the key idea is that AllocaInst contains all possible 
      //allocation instructions, we just need to see what is not supported.
      if (AllocaInst *AI = dyn_cast<AllocaInst>(I))
        //HexType checks if this allocation is an struct type or array type.
        if (HexTypeUtilSet->isInterestingType(AI->getAllocatedType())) {
          //Paul: if this alloca is safe than this will be not added and the 
          //object created in this allocation will be not traced, this is one of the 
          //optimizations of HexType. We need somethink similar for 
          //poly objects which will be not casted to non-poly objects
          if (ClSafeStackOpt && HexTypeUtilSet->isSafeStackAlloca(AI))
            return;
          AllAllocaWithFnSet.insert(
            std::pair<Instruction *, Function *>(
              dyn_cast<Instruction>(I), AI->getParent()->getParent()));
          //store this AI: allocation instruction
          AllAllocaSet.push_back(AI);
        }
    }
    
    //Paul: add tracing for an alloca instruction
    //also add start and end times for tracing this instruction
    void handleAllocaAdd(Module &M) {
      for (AllocaInst *AI : AllAllocaSet) {
        //Paul: get the next parent instruction of this instruction
        Instruction *next = HexTypeUtilSet->findNextInstruction(AI);
        IRBuilder<> Builder(next);

        Value *ArraySizeF = NULL;
        if (ConstantInt *constantSize =
            dyn_cast<ConstantInt>(AI->getArraySize()))
          ArraySizeF =
            ConstantInt::get(HexTypeUtilSet->Int64Ty,
                             constantSize->getZExtValue());
        else {
          Value *ArraySize = AI->getArraySize();
          if (ArraySize->getType() != HexTypeUtilSet->Int64Ty)
            ArraySizeF = Builder.CreateIntCast(ArraySize,
                                               HexTypeUtilSet->Int64Ty,
                                               false);
          else
            ArraySizeF = ArraySize;
        }

        Type *AllocaType = AI->getAllocatedType();
        StructElementInfoTy offsets;
        HexTypeUtilSet->getArrayOffsets(AllocaType, offsets, 0);
        if(offsets.size() == 0) continue;

        std::map<AllocaInst *, IntrinsicInst *>::iterator LifeTimeStart, LifeTimeEnd;
        LifeTimeStart = LifeTimeStartSet.begin();
        LifeTimeEnd = LifeTimeStartSet.end();
        bool UseLifeTimeInfo = false;
        
        //Paul: iterate trough all the instructions of an alloca instruction
        for (; LifeTimeStart != LifeTimeEnd; LifeTimeStart++)
          //Paul: 
          if (LifeTimeStart->first == AI) {
            IRBuilder<> BuilderAI(LifeTimeStart->second);
            //Paul: insert an object info stack update using BuilderAI
            //Paul: emit a tracing function for this instruction
            HexTypeUtilSet->insertUpdate(&M, BuilderAI, "__update_stack_oinfo",
                                         AI, offsets,
                                         HexTypeUtilSet->DL.getTypeAllocSize(
                                           AllocaType),
                                         ArraySizeF, NULL, NULL, AllocaType);
            UseLifeTimeInfo = true;
          }

        if (UseLifeTimeInfo)
          continue;
        
        //Paul: insert an object info stack update using Builder
        //Paul: emit a tracing function for this instruction
        HexTypeUtilSet->insertUpdate(&M, Builder, "__update_stack_oinfo",
                                     AI, offsets,
                                     HexTypeUtilSet->DL.getTypeAllocSize(
                                       AllocaType),
                                     ArraySizeF, NULL, NULL, AllocaType);
      }
    }
    
    //Paul: find all return instructions of a function
    void findReturnInsts(Function *f) {
      std::vector<Instruction*> *TempInstSet = new std::vector<Instruction *>;
      for (inst_iterator j = inst_begin(f), E = inst_end(f); j != E; ++j)
        if (isa<ReturnInst>(&*j))
          TempInstSet->push_back(&*j);

      ReturnInstSet.insert(std::pair<Function *,
                           std::vector<Instruction *>*>(f, TempInstSet));
    }
    
    //Paul: handle an alloca delete
    void handleAllocaDelete(Module &M) {
      std::map<Instruction *, Function *>::iterator LocalBegin, LocalEnd;
      LocalBegin = AllAllocaWithFnSet.begin();
      LocalEnd = AllAllocaWithFnSet.end();

      for (; LocalBegin != LocalEnd; LocalBegin++){
        Instruction *TargetInst = LocalBegin->first;
        AllocaInst *TargetAlloca = dyn_cast<AllocaInst>(TargetInst);

        Function *TargetFn = LocalBegin->second;

        std::vector<Instruction *> *FnReturnSet;
        FnReturnSet = ReturnInstSet.find(TargetFn)->second;

        std::vector<Instruction *>::iterator ReturnInstCur, ReturnInstEnd;
        ReturnInstCur = FnReturnSet->begin();
        ReturnInstEnd = FnReturnSet->end();
        DominatorTree dt = DominatorTree(*TargetFn);

        bool returnAI = false;
        for (; ReturnInstCur != ReturnInstEnd; ReturnInstCur++)
          if (dt.dominates(TargetInst, *ReturnInstCur)) {
            ReturnInst *returnValue = dyn_cast<ReturnInst>(*ReturnInstCur);
            if (returnValue->getNumOperands() &&
                returnValue->getOperand(0) == TargetAlloca) {
              returnAI = true;
              break;
            }
          }

        if (returnAI)
          continue;

        std::map<AllocaInst *, IntrinsicInst *>::iterator LifeTimeStart,
          LifeTimeEnd;
        LifeTimeStart = LifeTimeEndSet.begin();
        LifeTimeEnd = LifeTimeEndSet.end();
        bool lifeTimeEndEnable = false;
        for (; LifeTimeStart != LifeTimeEnd; LifeTimeStart++)
          if (LifeTimeStart->first == TargetAlloca) {
            IRBuilder<> BuilderAI(LifeTimeStart->second);
            HexTypeUtilSet->emitRemoveInst(&M, BuilderAI, TargetAlloca);
            lifeTimeEndEnable = true;
          }

        if (lifeTimeEndEnable)
          continue;

        ReturnInstCur = FnReturnSet->begin();
        ReturnInstEnd = FnReturnSet->end();

        for (; ReturnInstCur != ReturnInstEnd; ReturnInstCur++)
          if (dt.dominates(TargetInst, *ReturnInstCur)) {
            IRBuilder<> BuilderAI(*ReturnInstCur);
            //Paul: emit removal instruction
            HexTypeUtilSet->emitRemoveInst(&M, BuilderAI, TargetAlloca);
          }
      }
    }

    // This is typesan's optimization method
    // to reduce stack object tracing overhead
    bool mayCast(Function *F, std::set<Function*> &visited, bool *isComplete) {
      // Externals may cast
      if (F->isDeclaration())
        return true;

      // Check previously processed
      auto mayCastIterator = mayCastMap.find(F);
      if (mayCastIterator != mayCastMap.end())
        return mayCastIterator->second;
      //Paul: insert the function in the visited set
      visited.insert(F);

      bool isCurrentComplete = true;
      //Paul: cast the function to a CG call graph
      for (auto &I : *(*CG)[F]) {
        return true;
        Function *calleeFunction = I.second->getFunction();
        // Default to true to avoid accidental bugs on API changes
        bool result = false;
        // Indirect call
        if (!calleeFunction) {
          result = true;
          // Recursion detected, do not process callee
        } else if (visited.count(calleeFunction)) {
          isCurrentComplete = false;
          // Explicit call to checker
        } else if (
          //Paul: return true if the callee has to do with casting
          //these are our cast verification functions
          calleeFunction->getName().find("__dynamic_casting_verification") !=
          StringRef::npos ||
          calleeFunction->getName().find("__type_casting_verification_changing") !=
          StringRef::npos ||
          calleeFunction->getName().find("__type_casting_verification") !=
          StringRef::npos) {
          result = true;
          // Check recursively
        } else {
          bool isCalleeComplete;
          result = mayCast(calleeFunction, visited, &isCalleeComplete);
          // Forbid from caching if callee was not complete (due to recursion)
          isCurrentComplete &= isCalleeComplete;
        }
        // Found a potentialy cast, report true
        if (result) {
          // Cache and report even if it was incomplete
          // Missing traversal can never flip it to not found
          //Paul: store the function in my cast map.
          mayCastMap.insert(std::make_pair(F, true));
          *isComplete = true;
          return true;
        }
      }

      // No cast found anywhere, report false
      // Do not cache negative results if current traversal
      // was not complete (due to recursion)
      /*if (isCurrentComplete) {
        mayCastMap.insert(std::make_pair(F, false));
        }*/
      // Report to caller that this traversal was incomplete
      *isComplete = isCurrentComplete;
      return false;
    }
    
    //Paul: check if it is a safe cast function
    bool isSafeStackFn(Function *F) {
      assert(F && "Function can't be null");

      std::set<Function*> visitedFunctions;
      bool tmp;
      //Paul: call the mayCast() function from above
      //these are the HexType verification functions
      //these functions will be excluded
      bool mayCurrentCast = mayCast(&*F, visitedFunctions, &tmp);
      mayCastMap.insert(std::make_pair(&*F, mayCurrentCast));
      //if false return false
      if (!mayCurrentCast)
        return false;

      return true;
    }
    
    //Paul: this func. is used for tracing objects allocated on the stack
    void stackObjTracing(Module &M) {
      for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F) {
        //Paul: if is not interesting for HexType then just continue
        //A function is not interesting if it has no name, the initial block is empty,
        //or its name starts with __init_global_object
        if(!HexTypeUtilSet->isInterestingFn(&*F))
          continue;
        // Apply stack optimization
        // check that the function is a safe stack function, else continue
        if (ClStackOpt && !isSafeStackFn(&*F))
          continue;
        //Paul: for example malloc is here handled
        handleFnPrameter(M, &*F);
        for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB)
          for (BasicBlock::iterator i = BB->begin(),
               ie = BB->end(); i != ie; ++i) {
            //Paul: attach life time info to a basic block
            //here we set the start or the end time depending 
            //on which instruction was here used
            collectLifeTimeInfo(&*i);
            //Paul: store allocation instruction information
            collectAllocaInstInfo(&*i);
          }
        //Paul: store all return instructions of a function
        //most likely this will be used to clean up the stack when leaving the stack.
        findReturnInsts(&*F);
      }
      //Paul: insert object tracing functions for allocation instructions declaration
      handleAllocaAdd(M);
      //Paul: insert object tracing functions for allocation instruction deletion
      //life time ends here so we need to clean these allocations up
      handleAllocaDelete(M);
    }

    void globalObjTracing(Module &M) {
      //Paul: create a function for tracing global objects.
      Function *FGlobal = setGlobalObjUpdateFn(M);
      BasicBlock *BBGlobal = BasicBlock::Create(M.getContext(),
                                                "entry", FGlobal);
      IRBuilder<> BuilderGlobal(BBGlobal);
      
      //Paul: iterate trough all global objects.
      for (GlobalVariable &GV : M.globals()) {
        //Paul: continue is the GV flobal variable is an obj. constructor, destructors, etc.
        if (GV.getName() == "llvm.global_ctors" ||
            GV.getName() == "llvm.global_dtors" ||
            GV.getName() == "llvm.global.annotations" ||
            GV.getName() == "llvm.used")
          continue;
        
        //Paul: it checks that thre GV: global variable is an array or struct allocation
        if (HexTypeUtilSet->isInterestingType(GV.getValueType())) {
          StructElementInfoTy offsets;
          Value *NElems = NULL;
          Type *AllocaType;

          if(isa<ArrayType>(GV.getValueType())) {
            ArrayType *AI = dyn_cast<ArrayType>(GV.getValueType());
            AllocaType = AI->getElementType();
            NElems = ConstantInt::get(HexTypeUtilSet->Int64Ty,
                                      AI->getNumElements());
          }
          else {
            AllocaType = GV.getValueType();
            NElems = ConstantInt::get(HexTypeUtilSet->Int64Ty, 1);
          }
          
          //Paul: returns the offsets of this allocation types and stores them in offsets.
          //the offsets are the DL date layout sizes of each of the components of this allocation type.
          HexTypeUtilSet->getArrayOffsets(AllocaType, offsets, 0);
          if(offsets.size() == 0) continue;
          
          //Paul: insert the update for the particular object type allocation and 
          //emit the object tracing function
          HexTypeUtilSet->insertUpdate(&M, BuilderGlobal,
                                       "__update_global_oinfo",
                                       &GV, offsets, HexTypeUtilSet->DL.
                                       getTypeAllocSize(AllocaType),
                                       NElems, NULL, BBGlobal, AllocaType);
        }
      }
      //Paul: set the return of this function to be void
      BuilderGlobal.CreateRetVoid();
      appendToGlobalCtors(M, FGlobal, 0);
    }
    
    //Paul: emit type info as global value
    void emitTypeInfoAsGlobalVal(Module &M) {
      std::string mname = M.getName();
      HexTypeUtilSet->syncModuleName(mname);

      char ParentSetGlobalValName[MAXLEN];

      strcpy(ParentSetGlobalValName, mname.c_str());
      strcat(ParentSetGlobalValName, ".hextypepass_cinfo");

      HexTypeUtilSet->typeInfoArrayGlobal =
        HexTypeUtilSet->emitAsGlobalVal(M, ParentSetGlobalValName,
                        &(HexTypeUtilSet->typeInfoArray));

    }

    //Paul: generic module start function
    virtual bool runOnModule(Module &M) {
      // init HexTypePass
      CG = &getAnalysis<CallGraphWrapperPass>().getCallGraph();
      HexTypeLLVMUtil HexTypeUtilSetT(M.getDataLayout());
      HexTypeUtilSet = &HexTypeUtilSetT;
      HexTypeUtilSet->initType(M);
      
      //Paul: 
      HexTypeUtilSet->createObjRelationInfo(M);
      if (HexTypeUtilSet->AllTypeInfo.size() > 0)
        emitTypeInfoAsGlobalVal(M);

      // Init for only tracing casting related objects
      if (ClCastObjOpt || ClCreateCastRelatedTypeList)
        //Paul: this is just a set on this object
        HexTypeUtilSet->setCastingRelatedSet();

      // Global object tracing
      //Paul: start annotating global obj for tracing and emit function for tracing them
      globalObjTracing(M);

      // Stack object tracing
      //Paul: stack obj tracing, allocation and deletion when on the object is freed, the free() 
      //function is called on it
      stackObjTracing(M);

      return false;
    }
  };
}

//register pass
char HexType::ID = 0;
INITIALIZE_PASS_BEGIN(HexType, "HexType",
                      "HexTypePass: fast type safety for C++ programs.",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(HexType, "HexType",
                    "HexTypePass: fast type safety for C++ programs.",
                    false, false)
ModulePass *llvm::createHexTypePass() {
  return new HexType();
}
