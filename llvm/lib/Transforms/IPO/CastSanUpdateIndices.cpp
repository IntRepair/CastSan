#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/CastSan.h"
#include "llvm/Transforms/IPO/CastSanLayoutBuilder.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/MDBuilder.h"

#include "llvm/Transforms/IPO/CastSanLog.h"
#include "llvm/Transforms/IPO/CastSanTools.h"

#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/HexTypeUtil.h"

#include <list>
#include <vector>
#include <set>
#include <map>
#include <math.h>
#include <algorithm>
#include <iostream>
#include <limits>

// you have to modify the following 4 files for each additional LLVM pass
// 1. include/llvm/IPO.h
// 2. lib/Transforms/IPO/IPO.cpp
// 3. include/llvm/LinkAllPasses.h
// 4. include/llvm/InitializePasses.h
// 5. lib/Transforms/IPO/PassManagerBuilder.cpp

using namespace llvm;

namespace {
  /**
   * Pass for updating the annotated instructions with the new indices
   Paul: the UpdateIndices pass just runs the runOnModule() function
   all the function called in this module reside in the next pass, Subst_Module.
   This pass adds the needed checks.
   */
  struct SDUpdateIndices : public ModulePass {
    static char ID; // Pass identification, replacement for typeid

    SDUpdateIndices() : ModulePass(ID) {
      sd_print("initializing SDUpdateIndices pass\n");
      initializeSDUpdateIndicesPass(*PassRegistry::getPassRegistry());
    }

    virtual ~SDUpdateIndices() {
      sd_print("deleting SDUpdateIndices pass\n");
    }

    bool runOnModule(Module &M) override {
      //Paul: first get the results from the previous layout builder pass 
      layoutBuilder = &getAnalysis<SDLayoutBuilder>();
      assert(layoutBuilder);

      //Paul: second get the results from the class hierarchy analysis pass
      cha = &getAnalysis<SDBuildCHA>();

      sd_print("\n P4. Started running the 4th pass (Update indices) ...\n");

      //Paul: substitute the old v table index witht the new one
      //Intrinsic::sd_get_vtbl_index -> Intrinsic::sd_subst_vtbl_index
      handleSDGetVtblIndex(&M); 
 
      //Paul: adds the range check (casted_vptr, start, width, alingment)
      //Intrinsic::sd_check_vtbl -> Intrinsic::sd_subst_check_range
      handleSDCheckVtbl(&M);  

      //Paul: add the range checks, success, failed path, the trap and replace the terminator   
      //Intrinsic::sd_get_checked_vptr ->  Intrinsic::sd_subst_check_range             
      handleSDGetCheckedVtbl(&M);            

      //Paul: this are for the additional v pointer which are not checked based on ranges 
      //Intrinsic::sd_get_vcall_index -> null (there is no substitution function used here)
      handleRemainingSDGetVcallIndex(&M);   

      handleCheckCast(M); 

      layoutBuilder->removeOldLayouts(M);    //Paul: remove old layouts
      layoutBuilder->clearAnalysisResults(); //Paul: clear all data structures holding analysis data

      sd_print("\n P4. Finished removing thunks from (Update indices) pass...\n");
      return true;
    }

    /*Paul: 
    this method is used to get analysis results on which this pass depends*/
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<SDLayoutBuilder>(); //Paul: depends on layout builder pass
      AU.addRequired<SDBuildCHA>(); //Paul: depends on CHA pass
      AU.addPreserved<SDBuildCHA>(); //Paul: should preserve the information from the CHA pass
    }

  private:
    SDLayoutBuilder* layoutBuilder;
    SDBuildCHA* cha;
    
    // metadata ids
    void handleSDGetVtblIndex(Module* M);
    void handleSDCheckVtbl(Module* M);
    void handleSDGetCheckedVtbl(Module* M);
    void handleRemainingSDGetVcallIndex(Module* M);
    void handleCheckCast(Module & M);
  };
}

/// ----------------------------------------------------------------------------
/// SDUpdateIndices implementation, this are executed inside P4. Next, P5 is executed.
/// ----------------------------------------------------------------------------

static std::string sd_getClassNameFromMD(llvm::MDNode* mdNode, unsigned operandNo = 0) {
//  llvm::MDTuple* mdTuple = dyn_cast<llvm::MDTuple>(mdNode);
//  assert(mdTuple);

  llvm::MDTuple* mdTuple = cast<llvm::MDTuple>(mdNode);
  assert(mdTuple->getNumOperands() > operandNo + 1);

//  llvm::MDNode* nameMdNode = dyn_cast<llvm::MDNode>(mdTuple->getOperand(operandNo).get());
//  assert(nameMdNode);
  llvm::MDNode* nameMdNode = cast<llvm::MDNode>(mdTuple->getOperand(operandNo).get());

//  llvm::MDString* mdStr = dyn_cast<llvm::MDString>(nameMdNode->getOperand(0));
//  assert(mdStr);
  llvm::MDString* mdStr = cast<llvm::MDString>(nameMdNode->getOperand(0));

  StringRef strRef = mdStr->getString();
  assert(sd_isVtableName_ref(strRef));

//  llvm::MDNode* gvMd = dyn_cast<llvm::MDNode>(mdTuple->getOperand(operandNo+1).get());
  llvm::MDNode* gvMd = cast<llvm::MDNode>(mdTuple->getOperand(operandNo+1).get());

//  SmallString<256> OutName;
//  llvm::raw_svector_ostream Out(OutName);
//  gvMd->print(Out, CURR_MODULE);
//  Out.flush();

  llvm::ConstantAsMetadata* vtblConsMd = dyn_cast_or_null<ConstantAsMetadata>(gvMd->getOperand(0).get());
  if (vtblConsMd == NULL) {
//    llvm::MDNode* tmpnode = dyn_cast<llvm::MDNode>(gvMd);
//    llvm::MDString* tmpstr = dyn_cast<llvm::MDString>(tmpnode->getOperand(0));
//    assert(tmpstr->getString() == "NO_VTABLE");

    return strRef.str();
  }

//  llvm::GlobalVariable* vtbl = dyn_cast<llvm::GlobalVariable>(vtblConsMd->getValue());
//  assert(vtbl);
  llvm::GlobalVariable* vtbl = cast<llvm::GlobalVariable>(vtblConsMd->getValue());

  StringRef vtblNameRef = vtbl->getName();
  assert(vtblNameRef.startswith(strRef));

  return vtblNameRef.str();
}

//Paul: this returns the v table index and puts it in a function 
// it uses this functions to get the old v table index and to substitute it 
//Intrinsic::sd_get_vtbl_index -> Intrinsic::sd_subst_vtbl_index
void SDUpdateIndices::handleSDGetVtblIndex(Module* M) {
  Function *sd_vtbl_indexF = M->getFunction(Intrinsic::getName(Intrinsic::sd_get_vtbl_index));

  // if the function doesn't exist, do nothing
  if (!sd_vtbl_indexF){
   return;
  }

  llvm::LLVMContext& C = M->getContext();
  Type* intType = IntegerType::getInt64Ty(C);

  // for each use of the function
  for (const Use &U : sd_vtbl_indexF->uses()) {
    
    // get the call inst
    llvm::CallInst* CI = cast<CallInst>(U.getUser());

    // get the old arguments
    //this is the vPointer 
    llvm::ConstantInt* vptr = dyn_cast<ConstantInt>(CI->getArgOperand(0));
    assert(vptr);

    llvm::MetadataAsValue* arg2 = dyn_cast<MetadataAsValue>(CI->getArgOperand(1));
    assert(arg2);

    MDNode* mdNode = dyn_cast<MDNode>(arg2->getMetadata());
    assert(mdNode);

    // first argument is the old vtable index
    int64_t oldIndex = vptr->getSExtValue();

    // second one is the tuple that contains the class name and the corresponding global var.
    // note that the global variable isn't always emitted
    // get the class name based on the mdNode of the second argument of the CI.
    // this class name was previously inserted here during code generation from CGVTable.cpp
    std::string className = sd_getClassNameFromMD(mdNode, 0);

    //retrieve the corresponding v table bassed on the class name.
    SDLayoutBuilder::vtbl_t classVtbl(className, 0);

    //do some printings of the classes and the associated v tables
    sd_print("\n C1: vptr callsite with classname: %s cha->knowsAbout(vtbl.first: %s, vtbl.second: %d) = bool: %d) \n", 
                                                               className.c_str(),
                                                         classVtbl.first.c_str(), 
                                                                classVtbl.second, 
                                                           cha->knowsAbout(classVtbl));

    // calculate the new index
    int64_t newIndex = layoutBuilder->translateVtblInd(classVtbl, oldIndex, true);

    // convert the integer to llvm value
    llvm::Value* newConsIntInd = llvm::ConstantInt::get(intType, newIndex);

    // since the result of the call instruction is i64, replace all of its occurence with this one
    IRBuilder<> B(CI);
    llvm::Value *Args[] = {newConsIntInd}; //this is the new constant index for the class v table.
    llvm::Value* newIntr = B.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::sd_subst_vtbl_index),
                                        Args);

    CI->replaceAllUsesWith(newIntr); //Paul: replace the old v table index with the new one 
    CI->eraseFromParent();
  }
}

//Paul: this is used for the in-place sort operation 
struct range_less_than_key {
  inline bool operator()(const SDLayoutBuilder::mem_range_t &r1, const SDLayoutBuilder::mem_range_t &r2) {
    return r1.second > r2.second; // Invert sign to sort in descending order
  }
};

//Paul: adds the range check (casted_vptr, start, width, alingment)
//add check v table and check v table range 
// it uses: 
// Intrinsic::sd_check_vtbl -> Intrinsic::sd_subst_check_range
void SDUpdateIndices::handleSDCheckVtbl(Module* M) {
  //in ItaniumCXXABI.CPP there was a call inserted to sd_check_vtbl which contains the class name 
  //of the object making the virtual function call
  Function *sd_vtbl_indexF = M->getFunction(Intrinsic::getName(Intrinsic::sd_check_vtbl));
  const DataLayout &DL = M->getDataLayout();
  llvm::LLVMContext& C = M->getContext();
  Type *IntPtrTy = DL.getIntPtrType(C, 0);

  // if the function doesn't exist, do nothing
  if (!sd_vtbl_indexF){
   return;
  }
    

  // for each use of the function
  for (const Use &U : sd_vtbl_indexF->uses()) {
    
    // get the call instruction from the uses 
    llvm::CallInst* CI = cast<CallInst>(U.getUser());

    // get the arguments
    llvm::Value* vptr = CI->getArgOperand(0);
    assert(vptr); //Paul: this is the v pointer
    
    //class name of the object which is making the call 
    llvm::MetadataAsValue* arg2 = dyn_cast<MetadataAsValue>(CI->getArgOperand(1));
    assert(arg2);

    MDNode* mdNode = dyn_cast<MDNode>(arg2->getMetadata());
    assert(mdNode);
    
    //precise class name of the object which is making the call, if exists
    llvm::MetadataAsValue* arg3 = dyn_cast<MetadataAsValue>(CI->getArgOperand(2));
    assert(arg3);

    MDNode* mdNode1 = dyn_cast<MDNode>(arg3->getMetadata());
    assert(mdNode1);

    // second one is the tuple that contains the class name and the corresponding global var.
    // note that the global variable isn't always emitted

    // class name of the calling object
    // this class name was previously inserted here during code generation from CGVTable.cpp
    std::string className = sd_getClassNameFromMD(mdNode, 0);

    //class name of the base class ?
    std::string preciseClassName = sd_getClassNameFromMD(mdNode1, 0);

    //declare a new v table with order number 0
    SDLayoutBuilder::vtbl_t vtbl(className, 0);

    llvm::Constant *start; //Paul: range start
    int64_t rangeWidth;    //Paul: range width

    sd_print("C2: Callsite for classname: %s cha->knowsAbout(vtbl.first: %s, vtbl.second: %d) = bool: %d) \n", 
                                                               className.c_str(),
                                                              vtbl.first.c_str(), 
                                                                     vtbl.second, 
                                                           cha->knowsAbout(vtbl));
    
    if (cha->knowsAbout(vtbl)) {
      if (preciseClassName != className) {
          sd_print("C2: Callsite for classname: %s base class: %s cha->knowsAbout(vtbl.first: %s, vtbl.second: %d) = bool: %d) \n",
                 className.c_str(),
                 preciseClassName.c_str(),
                 vtbl.first.c_str(),
                 vtbl.second,
                 cha->knowsAbout(vtbl));
     }
    }

    //in case there is a more precise class name (base class)
    if (cha->knowsAbout(vtbl)) {
      if (preciseClassName != className) {
        sd_print("C2: More precise class name = %s\n", preciseClassName.c_str());
        int64_t ind = cha->getSubVTableIndex(preciseClassName, className);
        sd_print("Index = %d \n", ind);
        if (ind != -1) {
          vtbl = SDLayoutBuilder::vtbl_t(preciseClassName, ind);
        }
      } 
    }

    //Paul: calculate the start address of the new v table
    if (cha->knowsAbout(vtbl) &&
       (!cha->isUndefined(vtbl) || cha->hasFirstDefinedChild(vtbl))) {
      
      // calculate the new index
      start = cha->isUndefined(vtbl) ?
        layoutBuilder->getVTableRangeStart(cha->getFirstDefinedChild(vtbl)) : //Paul: first child or first v table
        layoutBuilder->getVTableRangeStart(vtbl);
      
      //Paul: cloud size represents the range width
      // It basically counts the number of children in that cloud. 
      // The children have to belong to the inheritance path,
      // This is not checked here or enforced.
      rangeWidth = cha->getCloudSize(vtbl.first); //count the number of children in the tree 
      sd_print(" [rangeWidth = %d start = %p]  \n", rangeWidth, start);
    } else {
      // This is a class we have no metadata about (i.e. doesn't have any
      // non-virtuall subclasses). In a fully statically linked binary we
      // should never be able to create an instance of this.
      start = NULL;
      rangeWidth = 0;
      //std::cerr << "Emitting empty range for " << vtbl.first << "," << vtbl.second << "\n";
      sd_print(" [ no metadata available ] \n");
    }

    LLVMContext& C = CI->getContext();
    
    //Paul: the start variable is not NULL
    if (start) {
      IRBuilder<> builder(CI);
      builder.SetInsertPoint(CI);//Paul: used to specifi insertion points

      std::cerr << "llvm.sd.callsite.range:" << rangeWidth << std::endl;
        
      // The shift here is implicit since rangeWidth is in terms of indices, not bytes
      llvm::Value *width    = llvm::ConstantInt::get(IntPtrTy, rangeWidth); //rangeWidth is here 0
      llvm::Type *Int8PtrTy = IntegerType::getInt8PtrTy(C);
      llvm::Value *castVptr = builder.CreateBitCast(vptr, Int8PtrTy); //create bitcast operation here 

      if(!cha->hasAncestor(vtbl)) {
        sd_print("%s\n", vtbl.first.data());
        assert(false);
      }

      SDLayoutBuilder::vtbl_name_t root = cha->getAncestor(vtbl);
      assert(layoutBuilder->alignmentMap.count(root));

      llvm::Constant* alignment = llvm::ConstantInt::get(IntPtrTy, layoutBuilder->alignmentMap[root]);
      llvm::Value *Args[] = {castVptr, start, width, alignment};

      //create a call instruction where we give over the above parameters.
      //we will be calling the function sd_subst_check_range witht the parameters, Args  
      llvm::Value* newIntr = builder.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::sd_subst_check_range), Args);

      CI->replaceAllUsesWith(newIntr); //Paul: add a new call instruction with rangeWidth = 0 
      CI->eraseFromParent();

    } else { //Paul: if start == NULL
      std::cerr << "llvm.sd.callsite.false:" << vtbl.first << "," << vtbl.second << std::endl;
      CI->replaceAllUsesWith(llvm::ConstantInt::getFalse(C));
      CI->eraseFromParent();
    }
  }
}

//Paul: add the range checks, success, failed path, the trap and replace the terminator 
//add checked v table pointer, add subst range and the trap if failed
//it uses:  
// Intrinsic::sd_get_checked_vptr ->  Intrinsic::sd_subst_check_range
void SDUpdateIndices::handleSDGetCheckedVtbl(Module* M) {
  //in ItaniumCXXABI.CPP there was a call inserted to sd_get_checked_vptr which contains the class name 
  //of the object making the virtual function call
  Function *sd_vtbl_indexF = M->getFunction(Intrinsic::getName(Intrinsic::sd_get_checked_vptr));
  const DataLayout &DL = M->getDataLayout(); //Paul: get data layout 
  llvm::LLVMContext& C = M->getContext();    //Paul: get the context
  Type *IntPtrTy = DL.getIntPtrType(C, 0);   //Paul: get the Int pointer type

  // if the function doesn't exist, do nothing
  if (!sd_vtbl_indexF){
   return;
  }

  // Paul: iterate through all function uses
  for (const Use &U : sd_vtbl_indexF->uses()) {
    
    // get each call instruction
    llvm::CallInst* CI = cast<CallInst>(U.getUser());

    // get the v ptr
    llvm::Value* vptr = CI->getArgOperand(0);
    assert(vptr);//assert not null
 
    //Paul: get second operand
    llvm::MetadataAsValue* arg2 = dyn_cast<MetadataAsValue>(CI->getArgOperand(1));
    assert(arg2);//assert not null

    //Paul: get the metadata of the second param
    MDNode* mdNode = dyn_cast<MDNode>(arg2->getMetadata());
    assert(mdNode);//assert not null
    
    //Paul: get the third parameter
    llvm::MetadataAsValue* arg3 = dyn_cast<MetadataAsValue>(CI->getArgOperand(2));
    assert(arg3);//assert not null

    //Paul: get the metadata of the third param 
    MDNode* mdNode1 = dyn_cast<MDNode>(arg3->getMetadata());
    assert(mdNode1);//assert not null

    // second one is the tuple that contains the class name and the corresponding global var.
    // note that the global variable isn't always emitted
    //get the class name class name from argument 1
    std::string className = sd_getClassNameFromMD(mdNode, 0);       

    //get a more precise class name from argument 2
    std::string preciseClassName = sd_getClassNameFromMD(mdNode1,0);
    SDLayoutBuilder::vtbl_t vtbl(className, 0);
    llvm::Constant *start;
    int64_t rangeWidth;

    sd_print("\n C3: Callsite for classname: %s cha->knowsAbout(vtbl.first: %s, vtbl.second: %d) = bool: %d)\n",
                                                                          className.c_str(),
                                                                          vtbl.first.c_str(), 
                                                                          vtbl.second, 
                                                                          cha->knowsAbout(vtbl));
  
    //Paul: check if the class hierarchy analysis knows about the v table 
    if (cha->knowsAbout(vtbl)) {
      if (preciseClassName != className) {
        sd_print("C3: More precise class name (base class) = %s\n", preciseClassName.c_str());
        int64_t ind = cha->getSubVTableIndex(preciseClassName, className);
        SDLayoutBuilder::vtbl_name_t n = preciseClassName;

        if (ind == -1) {
          //className is the derive and the preciseClassName is the base class 
          ind = cha->getSubVTableIndex(className, preciseClassName);
          n = className;
        }

        if (ind != -1) {
          vtbl = SDLayoutBuilder::vtbl_t(n, ind);
        }
        sd_print("Index = %d \n", ind);
      } else{
        sd_print("There is no base class for this call site \n");
      }
    }
    sd_print("\n"); //just add a gap in the printings 

    LLVMContext& C = CI->getContext();                    //Paul: get call inst. context 
    llvm::BasicBlock *BB = CI->getParent();               //Paul: get the parent 
    llvm::Function *F = BB->getParent();                  //Paul: get the parent of the previous BB 
    llvm::Type *Int8PtrTy = IntegerType::getInt8PtrTy(C); //Paul: convert the context to  

    //Paul: split the success BB
    llvm::BasicBlock *SuccessBB = BB->splitBasicBlock(CI, "sd.vptr_check.success");
    //Paul: get the old BB terminator 
    llvm::Instruction *oldTerminator = BB->getTerminator();
    IRBuilder<> builder(oldTerminator);

    //do a bit cast and store the result in castVptr
    llvm::Value *castVptr = builder.CreateBitCast(vptr, Int8PtrTy);
 
    //Paul: layout builder has a memory range for that v table 
    if (layoutBuilder->hasMemRange(vtbl)) {
      if(!cha->hasAncestor(vtbl)) {
        sd_print("%s\n", vtbl.first.data());
        assert(false);
      }
      
      //get the root of this v table 
      SDLayoutBuilder::vtbl_name_t root = cha->getAncestor(vtbl);
      assert(layoutBuilder->alignmentMap.count(root));

      //determine the alignment value 
      llvm::Constant* alignment = llvm::ConstantInt::get(IntPtrTy, layoutBuilder->alignmentMap[root]);
      
      int i = 0;

      //notice a v table can have multiple ranges 
      std::vector<SDLayoutBuilder::mem_range_t> ranges(layoutBuilder->getMemRange(vtbl));
      std::sort(ranges.begin(), ranges.end(), range_less_than_key()); //Paul: sort the elements in the range 

      uint64_t sum = 0;
      //Paul: iterate throught the ranges and compute width 
      // in oder to insert the check we need only to know the start address and the width
      for (auto rangeIt : ranges) {
        sum += rangeIt.second; //Paul: compute the width of the range 
      }
      
      //printing some statistics 
      sd_print("For vTable: {%s , %d } emitting: %d range check(s) with total width of the ranges: %d \n", 
                                       vtbl.first.c_str(), 
                                       vtbl.second, 
                                       ranges.size(), 
                                       sum);
  
      //Paul: iterate throught the ranges for one v table at a time 
      for (auto rangeIt : ranges) {
        llvm::Value *start = rangeIt.first;
        llvm::Value *width = llvm::ConstantInt::get(IntPtrTy, rangeIt.second);
        llvm::Value *Args[] = {castVptr, start, width, alignment};
   
        //Paul: create the fast path success, this Intrinsic::sd_subst_check_range function
        // was previously added during code generation 
        //create a call to named fast path success 
        llvm::Value* fastPathSuccess = builder.CreateCall(Intrinsic::getDeclaration(M,
                                                     Intrinsic::sd_subst_check_range),
                                                                                Args);

        char blockName[256];
        
        //give a name to the failed block and attach an increment value to it, i
        snprintf(blockName, sizeof(blockName), "sd.fastcheck.fail.%d", i);

        //Paul: create the fast path check failed block 
        //F is the parent block of the current instructon block making the call to Intrinsic::sd_get_checked_vptr
        llvm::BasicBlock *fastCheckFailed = llvm::BasicBlock::Create(F->getContext(), blockName, F);
        
        //Paul: create the the conditional branch and add fast path success Call, success BB and fast check failed BB
        llvm::BranchInst *BI = builder.CreateCondBr(fastPathSuccess, SuccessBB, fastCheckFailed);
        llvm::MDBuilder MDB(BI->getContext());

        //Paul: set the branch weights 
        BI->setMetadata(LLVMContext::MD_prof, MDB.createBranchWeights(
                                              std::numeric_limits<uint32_t>::max(),
                                              std::numeric_limits<uint32_t>::min()));

        //Paul: set the insertion point 
        builder.SetInsertPoint(fastCheckFailed); //Paul: builder set the insertion point
        i++;
      }
    }

    /*
    llvm::BasicBlock *checkFailed = llvm::BasicBlock::Create(F->getContext(), "sd.check.fail", F);
    llvm::Type* argTs[] = { Int8PtrTy, Int8PtrTy };
    llvm::FunctionType *vptr_safeT = llvm::FunctionType::get(llvm::Type::getInt1Ty(C), argTs, false);
    llvm::Constant *vptr_safeF = M->getOrInsertFunction("_Z9vptr_safePKvPKc", vptr_safeT);
    llvm::Value* slowPathSuccess = builder.CreateCall2(
                vptr_safeF,
                castVptr,
                builder.CreateGlobalStringPtr(className));
                // TODO: Add dynamic class name to _vptr_safe as well

    BranchInst *BI = builder.CreateCondBr(slowPathSuccess, SuccessBB, checkFailed);
    llvm::MDBuilder MDB(BI->getContext());
    BI->setMetadata(LLVMContext::MD_prof, MDB.createBranchWeights(
      std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint32_t>::min()));

    builder.SetInsertPoint(checkFailed);
    */
    // Insert Check Failure
    builder.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::trap)); //Paul: insert the check failure trap 
   
    //Paul: this is an IRBuilder object delclared before the previous for loop
    //create unrechable code here. 
    builder.CreateUnreachable();

    oldTerminator->eraseFromParent();//Paul: remove old terminator
    CI->replaceAllUsesWith(vptr);//Paul: replace all uses with the new v pointer
    CI->eraseFromParent();
  } //end of all uses for loop.
}

//Paul: read the v call index and add replace all uses with this new value 
//it uses: 
// Intrinsic::sd_get_vcall_index -> null 
void SDUpdateIndices::handleRemainingSDGetVcallIndex(Module* M) {
  Function *sd_vcall_indexF = M->getFunction(Intrinsic::getName(Intrinsic::sd_get_vcall_index));

  // if the function doesn't exist, do nothing
  if (!sd_vcall_indexF){
   return;
  }

  // for each use of the function
  for (const Use &U : sd_vcall_indexF->uses()) {
    
    // get the call inst
    llvm::CallInst* CI = cast<CallInst>(U.getUser());

    // get the arguments, Paul: this argument is the v pointer.
    llvm::ConstantInt* arg1 = dyn_cast<ConstantInt>(CI->getArgOperand(0));
    assert(arg1); 

    // since the result of the call instruction is i64, replace all of its occurence with this one
    CI->replaceAllUsesWith(arg1);
  }
}

void SDUpdateIndices::handleCheckCast(Module & M) {
  Function * cast_info = M.getFunction(Intrinsic::getName(Intrinsic::cast_info));
  const DataLayout &DL = M.getDataLayout();
  llvm::LLVMContext& C = M.getContext();
  auto BoolTy = Type::getInt1Ty(C);
  auto Int64Ty = Type::getInt64Ty(C);
  auto Int8Ty = Type::getInt8Ty(C);
  Type *IntPtrTy = DL.getIntPtrType(C, 0);

  if(cast_info) {
  for(const Use & U : cast_info->uses()) {
    llvm::CallInst * CI = cast<CallInst>(U.getUser());
 
    // get the VTable Base Class Metadata
    llvm::MetadataAsValue* arg1 = dyn_cast<MetadataAsValue>(CI->getArgOperand(0));
    assert(arg1);

    MDNode* mdNode1 = dyn_cast<MDNode>(arg1->getMetadata());
    assert(mdNode1);

    std::string vBaseClassName = sd_getClassNameFromMD(mdNode1, 0);
 
    // get the Precise Class Metadata
    llvm::MetadataAsValue* arg2 = dyn_cast<MetadataAsValue>(CI->getArgOperand(1));
    assert(arg2);

    MDNode* mdNode2 = dyn_cast<MDNode>(arg2->getMetadata());
    assert(mdNode2);

    std::string preciseClassName = sd_getClassNameFromMD(mdNode2, 0);

    // get the vTable pointer
    llvm::Value* vptr = CI->getArgOperand(2);
    assert(vptr);

    std::cerr << "CastCheck: Checking cast to: " << preciseClassName << " using VTable with root: " << vBaseClassName << std::endl;

    // get VTable info from CastSan
    // first the vtable we want to add the range check in
    SDLayoutBuilder::vtbl_t vtbl(vBaseClassName, 0);

    llvm::Constant *start;
    int64_t rangeWidth;

    // then get the more precise subtree in the vtable tree
    if (cha->knowsAbout(vtbl)) {
      if (preciseClassName != vBaseClassName) {
        int64_t ind = cha->getSubVTableIndex(preciseClassName, vBaseClassName);
        std::cerr << "CastCheck: Index = " << ind << std::endl;
        if (ind != -1) {
          vtbl = SDLayoutBuilder::vtbl_t(preciseClassName, ind);
        }
      } 
    }
    else
    {
      std::cerr << "CastCheck: cha does not know about vtbl :(" << std::endl;
    }

    // the following is mainly the same as handleSDCheckVtbl() above:
    // get the start of the part of the vtable, calculate the range as count of all children of the root
    // ensure we got the vptr as int8ptr, get the alignement and put everything back in a Intrinsic
    if (cha->knowsAbout(vtbl) &&
       (!cha->isUndefined(vtbl) || cha->hasFirstDefinedChild(vtbl))) {
      
      start = cha->isUndefined(vtbl) ?
        layoutBuilder->getVTableRangeStart(cha->getFirstDefinedChild(vtbl)) :
        layoutBuilder->getVTableRangeStart(vtbl);
      
      rangeWidth = cha->getCloudSize(vtbl.first);
      std::cerr << "CastCheck: [rangeWidth = " << rangeWidth << " start = " << start << "] " << std::endl;
    } else {
      start = NULL;
      rangeWidth = 0;
      std::cerr << "CastCheck: [ no metadata available ] " << std::endl;
    }

    LLVMContext& C = CI->getContext();
    
    if (start) {
      IRBuilder<> builder(CI);
      builder.SetInsertPoint(CI);
        
      llvm::Value *width    = llvm::ConstantInt::get(IntPtrTy, rangeWidth);
      llvm::Type *Int8PtrTy = IntegerType::getInt8PtrTy(C);
      llvm::Value *castVptr = builder.CreateBitCast(vptr, Int8PtrTy); 

      // we do not have the root of the VTable: break up.
      if(!cha->hasAncestor(vtbl)) {
        std::cerr << vtbl.first.data() << std::endl;
        assert(false);
      }

      SDLayoutBuilder::vtbl_name_t root = cha->getAncestor(vtbl);
      assert(layoutBuilder->alignmentMap.count(root));

      int64_t alignmentBits = floor(log(layoutBuilder->alignmentMap[root] + 0.5)/log(2.0));
      llvm::Constant* alignment = llvm::ConstantInt::get(IntPtrTy, alignmentBits);
      llvm::Constant* alignment_r = llvm::ConstantInt::get(IntPtrTy, DL.getPointerSizeInBits(0) - alignmentBits);

      llvm::Constant* rootVtblInt    = dyn_cast<llvm::Constant>(start->getOperand(0));
      llvm::GlobalVariable* rootVtbl = dyn_cast<llvm::GlobalVariable>(rootVtblInt->getOperand(0));
      llvm::ConstantInt* startOff    = dyn_cast<llvm::ConstantInt>(start->getOperand(1));

      std::cerr << "CastCheck: Putting subst_cast_check in!" << std::endl;
      
      if (validConstVptr(rootVtbl, startOff->getSExtValue(), rangeWidth, DL, castVptr, 0)) {
	      
	      CI->replaceAllUsesWith(llvm::ConstantInt::getTrue(C));
	      CI->eraseFromParent();
      } else if (rangeWidth > 1) {
	      llvm::Value *Args[] = {start, width, alignment, alignment_r, castVptr};
	      Function *castCheckFunction =
		      (Function*)M.getOrInsertFunction(
			      "__type_casting_verification_ranged", BoolTy,
			      Int64Ty, Int64Ty, Int64Ty, Int64Ty, Int8PtrTy, nullptr);
	      llvm::Value* newIntrCast = builder.CreateCall(castCheckFunction, Args);
	      
	      CI->replaceAllUsesWith(newIntrCast);
	      CI->eraseFromParent();
      } else {
	      llvm::Value *Args[] = {start, castVptr};
	      Function *castCheckFunction =
		      (Function*)M.getOrInsertFunction(
			      "__type_casting_verification_equal", BoolTy,
			      Int64Ty, Int8PtrTy, nullptr);
	      llvm::Value* newIntrCast = builder.CreateCall(castCheckFunction, Args);
	      CI->replaceAllUsesWith(newIntrCast);
	      CI->eraseFromParent();
      }
      /*if (rangeWidth > 1) {
        // The shift here is implicit since rangeWidth is in terms of indices, not bytes

        // Rotate right by 3 to push the lowest order bits into the higher order bits
        llvm::Value *vptrInt = builder.CreatePtrToInt(castVptr, IntPtrTy);
        llvm::Value *startInt = builder.CreatePtrToInt(start, IntegerType::getInt64Ty(C));

        llvm::Value *diff = builder.CreateSub(vptrInt, startInt);
        llvm::Value *diffShr = builder.CreateLShr(diff, 3);
        llvm::Value *diffShl = builder.CreateShl(diff, DL.getPointerSizeInBits(0) - 3);
        llvm::Value *diffRor = builder.CreateOr(diffShr, diffShl);

        llvm::Value *inRange = builder.CreateICmpULE(diffRor, width);
          
        CI->replaceAllUsesWith(inRange);
        CI->eraseFromParent();
      } else {
        llvm::Value *startInt = builder.CreatePtrToInt(start, IntegerType::getInt64Ty(C));
        llvm::Value *vptrInt = builder.CreatePtrToInt(castVptr, IntPtrTy);
        llvm::Value *inRange = builder.CreateICmpEQ(vptrInt, startInt);

        CI->replaceAllUsesWith(inRange);
        CI->eraseFromParent();
      }*/

    } else {
	    std::cerr << "CastCheck: llvm.sd.callsite.false:" << vtbl.first << "," << vtbl.second << std::endl;
	    CI->replaceAllUsesWith(llvm::ConstantInt::getFalse(C));
	    CI->eraseFromParent();
    }
  }
  }
  HexTypeLLVMUtil HexTypeUtilSet(M.getDataLayout());
  HexTypeUtilSet.initType(M);
  
  HexTypeUtilSet.createObjRelationInfo(M);
  CastSanUtil & CastSan = HexTypeUtilSet.CastSan;								  
  
  Function * subst_dynamic_castF = M.getFunction("__dynamic_casting_verification");
  if (subst_dynamic_castF) {
	  for (const Use &U : subst_dynamic_castF->uses()) {
		  llvm::CallInst* CI = cast<CallInst>(U.getUser());
		  IRBuilder<> builder(CI);
		  
		  ConstantInt * ConstDstTypeHash = dyn_cast<ConstantInt>(CI->getArgOperand(2));
		  ConstantInt * ConstSrcTypeHash = dyn_cast<ConstantInt>(CI->getArgOperand(1));
		  
		  uint64_t DstTypeH = ConstDstTypeHash->getZExtValue();
		  uint64_t SrcTypeH = ConstSrcTypeHash->getZExtValue();
		  
		  auto DstType = CastSan.Types[DstTypeH];
		  auto SrcType = CastSan.Types[SrcTypeH];
		  
		  auto DstMangledName = DstType.MangledName;
		  auto SrcMangledName = SrcType.MangledName;

		  std::cerr << "Cast from " << SrcMangledName << " to " << DstMangledName << std::endl;
		  
		  assert (DstType.Polymorphic && SrcType.Polymorphic && "Dynamic cast is not possible");
		  
		  auto BaseType = &SrcType;
		  
		  // find primary polymorphic parent
		  while (BaseType->Parents.size()) {
			  bool noPolyParent = true;
			  for (auto parent : BaseType->Parents) {
				  if (parent->Polymorphic) {
					  BaseType = parent;
					  noPolyParent = false;
					  break;
				  }
			  }
			  if (noPolyParent)
				  break;
		  }
		  
		  // get VTable info from CastSan
		  // first the vtable we want to add the range check in
		  SDLayoutBuilder::vtbl_t vtbl(BaseType->MangledName, 0);
		  
		  llvm::Constant *start;
		  int64_t rangeWidth;
		  
		  // then get the more precise subtree in the vtable tree
		  if (cha->knowsAbout(vtbl)) {
			  if (BaseType->MangledName.compare(DstType.MangledName) != 0) {

				  int64_t ind = cha->getSubVTableIndex(DstType.MangledName, BaseType->MangledName);
				  std::cerr << "CastCheck: Index = " << ind << std::endl;
				  if (ind != -1) {
					  vtbl = SDLayoutBuilder::vtbl_t(DstType.MangledName, ind);
				  }
			  } 
		  }
		  else
		  {
			  std::cerr << "CastCheck: cha does not know about vtbl :(" << std::endl;
		  }
		  
		  // the following is mainly the same as handleSDCheckVtbl() above:
		  // get the start of the part of the vtable, calculate the range as count of all children of the root
		  // ensure we got the vptr as int8ptr, get the alignement and put everything back in a Intrinsic
		  if (cha->knowsAbout(vtbl) &&
		      (!cha->isUndefined(vtbl) || cha->hasFirstDefinedChild(vtbl))) {
                          
			  start = cha->isUndefined(vtbl) ?
                                 layoutBuilder->getVTableRangeStart(cha->getFirstDefinedChild(vtbl)) :
                                 layoutBuilder->getVTableRangeStart(vtbl);

			  rangeWidth = cha->getCloudSize(vtbl.first);
			  std::cerr << "CastCheck: [rangeWidth = " << rangeWidth << " start = " << start << "] " << std::endl;
		  } else {
			  start = NULL;
			  rangeWidth = 0;
			  std::cerr << "CastCheck: [ no metadata available ] " << std::endl;
		  }
		  
		  LLVMContext& C = CI->getContext();
		  
		  if (start) {
			  IRBuilder<> builder(CI);
			  builder.SetInsertPoint(CI);

			  if (rangeWidth > 1)
			  {
				  llvm::Value *width    = llvm::ConstantInt::get(IntPtrTy, rangeWidth);
				  llvm::Type *Int8PtrTy = IntegerType::getInt8PtrTy(C);
				  
				  // we do not have the root of the VTable: break up.
				  if(!cha->hasAncestor(vtbl)) {
					  std::cerr << vtbl.first.data() << std::endl;
					  assert(false);
				  }
				  
				  SDLayoutBuilder::vtbl_name_t root = cha->getAncestor(vtbl);
				  assert(layoutBuilder->alignmentMap.count(root));
				  
				  int64_t alignmentBits = floor(log(layoutBuilder->alignmentMap[root] + 0.5)/log(2.0));
				  llvm::Constant* alignment = llvm::ConstantInt::get(IntPtrTy, alignmentBits);
				  llvm::Constant* alignment_r = llvm::ConstantInt::get(IntPtrTy, DL.getPointerSizeInBits(0) - alignmentBits);
				  
				  llvm::Constant* rootVtblInt    = dyn_cast<llvm::Constant>(start->getOperand(0));
				  llvm::GlobalVariable* rootVtbl = dyn_cast<llvm::GlobalVariable>(rootVtblInt->getOperand(0));
				  llvm::ConstantInt* startOff    = dyn_cast<llvm::ConstantInt>(start->getOperand(1));
				  
				  std::cerr << "CastCheck: Changing dynamic_cast arguments!" << std::endl;
				  
				  CI->setArgOperand(1, start);
				  CI->setArgOperand(2, width);
				  CI->setArgOperand(3, alignment);
				  CI->setArgOperand(4, alignment_r);
			  } else {
				  auto src = CI->getArgOperand(0);
				  auto off = CI->getArgOperand(5);
				  
				  Function *dynCastEqualFunction =
					  (Function*)M.getOrInsertFunction(
						  "__dynamic_casting_verification_equal", CI->getType(),
						  src->getType(), HexTypeUtilSet.Int64Ty, off->getType(), nullptr);
				  Value *Param[3] = { src, start, off };
				  Value * dynCastEqual = builder.CreateCall(dynCastEqualFunction, Param);
				  CI->replaceAllUsesWith(dynCastEqual);//Paul: write the v pointer back 
				  CI->eraseFromParent();
				  
			  }
		  }
	  }
	  
  } else {	  
	  std::cerr << "No dynamic casting functions.... " << std::endl;
  }
}


  /**
   * P5 Module pass for substittuing the final subst_ intrinsics
   */
  class SDSubstModule : public ModulePass {
  public:
    static char ID; // Pass identification, replacement for typeid

    SDSubstModule() : ModulePass(ID) {
      sd_print("initializing SDSubstModule pass\n");
      initializeSDSubstModulePass(*PassRegistry::getPassRegistry());
    }

    virtual ~SDSubstModule() {
      sd_print("deleting SDSubstModule pass\n");
    }

    bool runOnModule(Module &M) {
      sd_print("\nP5. Started running SDSubstModule pass ...\n");
      sd_print("P5. Starting final range checks additions ...\n");
      
      //Paul: count the number of indexes substituted
      int64_t indexSubst = 0;

      //Paul: count number of range substituted
      int64_t rangeSubst = 0;


      //Paul: count number of equalities substituted
      int64_t eqSubst = 0; 


      //Paul: count the number of constant pointers
      int64_t constPtr = 0;


      //Paul: cum up the width of a range such that
      // we can compute an average value for each inserted check
      uint64_t sumWidth = 0.0;

      //Paul: substitute the v table index
      //get the function used to substitute the v table index 
      Function *sd_subst_indexF = M.getFunction(Intrinsic::getName(Intrinsic::sd_subst_vtbl_index));

       //Paul: substitute the v table range  
       //get the function used to subsitute the range check 
       Function *sd_subst_rangeF = M.getFunction(Intrinsic::getName(Intrinsic::sd_subst_check_range));

      // Paul: write the v pointer value back from all the functions which will
      // be called based on this pointer
      if (sd_subst_indexF) {
        for (const Use &U : sd_subst_indexF->uses()) {
          // get the call inst,
          //Returns the User that contains this Use.
          //For an instruction operand, for example, this will return the instruction.
          llvm::CallInst* CI = cast<CallInst>(U.getUser());//Paul: read this value back from code 

          // Paul: get the first arguments, this is the v pointer
          llvm::ConstantInt* arg1 = dyn_cast<ConstantInt>(CI->getArgOperand(0));
          assert(arg1);
          CI->replaceAllUsesWith(arg1);//Paul: write the v pointer back 
          CI->eraseFromParent();

          //count the total number of v pointer index substitutions 
          indexSubst += 1; 
        }
      }

      const DataLayout &DL = M.getDataLayout();
      LLVMContext& C = M.getContext(); 
      Type *IntPtrTy = DL.getIntPtrType(C, 0);
      
      //Paul: add the final range checks 
      //Notice: that we have ranges with: width > 1 or < 1
      if (sd_subst_rangeF) {
        
        //coun number of ranges added
        int rangeCounter = 0;

        //count number of constant checks added
        int constantCounter = 0;

        //Paul: for all the places where the range check has to be added
        for (const Use &U : sd_subst_rangeF->uses()) {
          
          // get the call inst
          llvm::CallInst* CI = cast<CallInst>(U.getUser());
          IRBuilder<> builder(CI);

          // get the arguments, this have been writen during the pass P4 from above 
          llvm::Value* vptr            = CI->getArgOperand(0);
          llvm::Constant* start        = dyn_cast<Constant>(CI->getArgOperand(1));
          llvm::ConstantInt* width     = dyn_cast<ConstantInt>(CI->getArgOperand(2));
          llvm::ConstantInt* alignment = dyn_cast<ConstantInt>(CI->getArgOperand(3));

          //all three values > 0
          assert(vptr && start && width);
          
          //get the value as a 64-bit unsigned integer after it has been sign extended
          //as appropriate for the type of this constant 
          int64_t widthInt = width->getSExtValue();
          int64_t alignmentInt = alignment->getSExtValue();
          int alignmentBits = floor(log(alignmentInt + 0.5)/log(2.0));

          llvm::Constant* rootVtblInt    = dyn_cast<llvm::Constant>(start->getOperand(0));
          llvm::GlobalVariable* rootVtbl = dyn_cast<llvm::GlobalVariable>(rootVtblInt->getOperand(0));
          llvm::ConstantInt* startOff    = dyn_cast<llvm::ConstantInt>(start->getOperand(1));
 
          //Paul: sum up all the ranges widths which will be substituted 
          //this is just for statistics relevant
          sumWidth = sumWidth + widthInt;

          //check if vptr is constant
          if (validConstVptr(rootVtbl, startOff->getSExtValue(), widthInt, DL, vptr, 0)) {
            
            //replace call instruction with an constant int 
            CI->replaceAllUsesWith(llvm::ConstantInt::getTrue(C));
            CI->eraseFromParent();
           
            //Paul: sum up how many times we had constant pointers 
            //this is used just for statistics.
            constPtr++;
          } else

          //Paul: if the range is grether than 1 do the rotation checks 
          if (widthInt > 1) {
            // create pointer to int
            llvm::Value *vptrInt = builder.CreatePtrToInt(vptr, IntPtrTy);
            
            //substract pointer from start address, start - vptrInt
            llvm::Value *diff = builder.CreateSub(vptrInt, start);
            
            //shift right diff with the number of alignmentBits
            llvm::Value *diffShr = builder.CreateLShr(diff, alignmentBits);

            //shift left diff with the number of DL.getPointerSizeInBits(0) - alignmentBits
            llvm::Value *diffShl = builder.CreateShl(diff, DL.getPointerSizeInBits(0) - alignmentBits);

            //create diff rotation 
            llvm::Value *diffRor = builder.CreateOr(diffShr, diffShl);
            
            //create comparison, diffRor <= width 
            llvm::Value *inRange = builder.CreateICmpULE(diffRor, width); //Paul: create a comparison expr.
            
            //replace the in range check 
            CI->replaceAllUsesWith(inRange);

            //CI remove from parent 
            CI->eraseFromParent();
            
            //count the number of range substitutions added
            rangeSubst += 1;
            
            sd_print("Range: %d has width: % d start: %d vptrInt: %d \n", rangeSubst, widthInt, start, vptrInt);

            //Paul: range = 1 or 0
          } else {
            llvm::Value *vptrInt = builder.CreatePtrToInt(vptr, IntPtrTy);

            //create comparison, v pointer == start  
            llvm::Value *inRange = builder.CreateICmpEQ(vptrInt, start);

            CI->replaceAllUsesWith(inRange);
            CI->eraseFromParent();
            
            eqSubst += 1; //count number of equalities substitutions added
          }        
        }
      }
     
      //finished adding all the range checks, now print some statistics.
      //in the interleaving paper the average number of ranges per call site was close to 1 (1,005).
      sd_print("\n P5. Finished running SDSubstModule pass...\n");

      sd_print("\n ---P5. SDSubst Statistics--- \n");

      sd_print(" Total index substitutions %d \n", indexSubst);
      sd_print(" Total range checks added %d \n", rangeSubst);
      sd_print(" Total eq_checks added %d \n", eqSubst);
      sd_print(" Total const_ptr % d \n", constPtr);
      sd_print(" Average width % lf \n", sumWidth * 1.0 / (rangeSubst + eqSubst + constPtr));

      //one of these values has to be > than 0 
      return indexSubst > 0 || rangeSubst > 0 || eqSubst > 0 || constPtr > 0;
    }

  };

char SDUpdateIndices::ID = 0;
char SDSubstModule::ID = 0;

INITIALIZE_PASS(SDSubstModule, "sdsdmp", "Module pass for substituting the constant-holding intrinsics generated by sdmp.", false, false)
INITIALIZE_PASS_BEGIN(SDUpdateIndices, "cc", "Change Constant", false, false)
INITIALIZE_PASS_DEPENDENCY(SDLayoutBuilder)
INITIALIZE_PASS_DEPENDENCY(SDBuildCHA)
INITIALIZE_PASS_END(SDUpdateIndices, "cc", "Change Constant", false, false)


ModulePass* llvm::createSDUpdateIndicesPass() {
  return new SDUpdateIndices();
}

ModulePass* llvm::createSDSubstModulePass() {
  return new SDSubstModule();
}

