//===- HexTypeUtil.cpp - helper functions and classes for HexType ---------===//
////
////                     The LLVM Compiler Infrastructure
////
//// This file is distributed under the University of Illinois Open Source
//// License. See LICENSE.TXT for details.
////
////===--------------------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/HexTypeUtil.h"
#include "llvm/Transforms/Utils/CastSanUtil.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>
#include <iostream>

#define MAXLEN 1000

namespace llvm {
  //Paul: next lines are some options all set on false initially
  cl::opt<bool> ClCreateCastRelatedTypeList(
    "create-cast-releated-type-list",
    cl::desc("create casting related object list"),
    cl::Hidden, llvm::cl::init(false));

  cl::opt<bool> ClStackOpt(
    "stack-opt", cl::desc("stack object optimization (from typesan)"),
    cl::Hidden, cl::init(false));

  cl::opt<bool> ClSafeStackOpt(
    "safestack-opt",
    cl::desc("stack object tracing optimization using safestack"),
    cl::Hidden, cl::init(false));

  cl::opt<bool> ClCastObjOpt(
    "cast-obj-opt",
    cl::desc("only casting related object tracing optimization"),
    cl::Hidden, cl::init(false));

  cl::opt<bool> ClInlineOpt(
    "inline-opt",
    cl::desc("reduce runtime library function call overhead"),
    cl::Hidden, cl::init(false));

  cl::opt<bool> ClCompileTimeVerifyOpt(
    "compile-time-verify-opt",
    cl::desc("compile time verification"),
    cl::Hidden, cl::init(false));

  cl::opt<bool> ClMakeLogInfo(
    "make-loginfo",
    cl::desc("create log information"),
    cl::Hidden, cl::init(false));

  cl::opt<bool> ClMakeTypeInfo(
    "make-typeinfo",
    cl::desc("create Type-hash information"),
    cl::Hidden, cl::init(false));
  
  //Paul: compute a crc hash value from a given string
  uint64_t crc64c(unsigned char *message) {
    int i, j;
    unsigned int byte;
    uint64_t crc, mask;
    static uint64_t table[256];

    if (table[1] == 0) {
      for (byte = 0; byte <= 255; byte++) {
        crc = byte;
        for (j = 7; j >= 0; j--) {    // Do eight times.
          mask = -(crc & 1);
          crc = (crc >> 1) ^ (0xC96C5795D7870F42UL & mask);
        }
        table[byte] = crc;
      }
    }

    i = 0;
    crc = 0xFFFFFFFFFFFFFFFFUL;
    while ((byte = message[i]) != 0) {
      crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF];
      i = i + 1;
    }
    return ~crc;
  }
  
  //Paul: string manipualtion
  void removeTargetStr(std::string& FullStr, std::string RemoveStr) {
    std::string::size_type i;

    while((i = FullStr.find(RemoveStr)) != std::string::npos) {
      if (RemoveStr.compare("::") == 0)
        FullStr.erase(FullStr.begin(), FullStr.begin() + i + 2);
      else
        FullStr.erase(i, RemoveStr.length());
    }
  }
  
  //Paul: string manipulation
  void removeTargetNum(std::string& TargetStr) {
    std::string::size_type i;
    if((i = TargetStr.find(".")) == std::string::npos)
      return;

    if((i+1 < TargetStr.size()) &&
       (TargetStr[i+1] >= '0' && TargetStr[i+1] <='9'))
      TargetStr.erase(i, TargetStr.size() - i);
  }
  
  //Paul: create a global value
  GlobalVariable* HexTypeLLVMUtil::emitAsGlobalVal(Module &M, char *GlobalVarName,
                                                   std::vector<Constant*> *TargetArray) {
    ArrayType *InfoArrayType = ArrayType::get(Int64Ty, TargetArray->size());
    Constant* infoArray = ConstantArray::get(InfoArrayType, *TargetArray);
    GlobalVariable* infoGlobal =
      new GlobalVariable(M, InfoArrayType, false,
                         GlobalVariable::LinkageTypes::ExternalLinkage,
                         nullptr, GlobalVarName);
    infoGlobal->setInitializer(infoArray);
    return infoGlobal;
  }
  
  //Paul:check if it is a safe stack allocation
  //this is used to not trace objects which are not used in object casts.
  // need to build something similar for our case where we want to not track
  //polymorphic objects which are involved in only polymorphic casts.
  bool HexTypeLLVMUtil::isSafeStackAlloca(AllocaInst *AI) {
    // Go through all uses of this alloca and check whether all accesses to
    // the allocated object are statically known to be memory safe and, hence,
    // the object can be placed on the safe stack.

    SmallPtrSet<const Value *, 16> Visited;
    SmallVector<const Instruction *, 8> WorkList;
    WorkList.push_back(AI);

    // A DFS search through all uses of the alloca in bitcasts/PHI/GEPs/etc.
    while (!WorkList.empty()) {
      const Instruction *V = WorkList.pop_back_val();
      for (const Use &UI : V->uses()) {
        auto I = cast<const Instruction>(UI.getUser());
        assert(V == UI.get());

        switch (I->getOpcode()) {
        case Instruction::Load:
          // Loading from a pointer is safe.
          break;
        case Instruction::VAArg:
          // "va-arg" from a pointer is safe.
          break;
        case Instruction::Store:
          if (V == I->getOperand(0)){
            //NHB odd exception for libc - lets see if it works
            if (I->getOperand(1) == AI &&
               !(AI->getAllocatedType()->isPointerTy()))
              break;
            // Stored the pointer - conservatively assume it may be unsafe.
            return false;
          }
          // Storing to the pointee is safe.
          break;
        case Instruction::GetElementPtr:
          // if (!cast<const GetElementPtrInst>(I)->hasAllConstantIndices())
          // GEP with non-constant indices can lead to memory errors.
          // This also applies to inbounds GEPs, as the inbounds attribute
          // represents an assumption that the address is in bounds,
          // rather than an assertion that it is.
          // return false;

          // We assume that GEP on static alloca with constant indices
          // is safe, otherwise a compiler would detect it and
          // warn during compilation.

          // NHB Todo: this hasn't come up in spec, but it's probably fine
          if (!isa<const ConstantInt>(AI->getArraySize())) {
            // However, if the array size itself is not constant, the access
            // might still be unsafe at runtime.
            return false;
          }
          /* fallthrough */
        case Instruction::BitCast:
        case Instruction::IntToPtr:
        case Instruction::PHI:
        case Instruction::PtrToInt:
        case Instruction::Select:
          // The object can be safe or not, depending on how the result of the
          // instruction is used.
          if (Visited.insert(I).second)
            WorkList.push_back(cast<const Instruction>(I));
          break;

        case Instruction::Call:
        case Instruction::Invoke: {
          // FIXME: add support for memset and memcpy intrinsics.
          ImmutableCallSite CS(I);

          if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(I)) {
            if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
                II->getIntrinsicID() == Intrinsic::lifetime_end)
              continue;
          }

          // LLVM 'nocapture' attribute is only set for arguments
          // whose address is not stored, passed around, or used in any other
          // non-trivial way.
          // We assume that passing a pointer to an object as a 'nocapture'
          // argument is safe.
          // FIXME: a more precise solution would require an interprocedural
          // analysis here, which would look at all uses of an argument inside
          // the function being called.
          ImmutableCallSite::arg_iterator B = CS.arg_begin(),
            E = CS.arg_end();
          for (ImmutableCallSite::arg_iterator A = B; A != E; ++A)
            /*NHB mod*/
            //!CS.doesNotCapture(A - B))
            if (A->get() == V && V->getType()->isPointerTy()) {
              // The parameter is not marked 'nocapture' - unsafe.
              return false;
            }
          continue;
        }
        default:
          // The object is unsafe if it is used in any other way.
          return false;
        }
      }
    }

    // All uses of the alloca are safe, we can place it on the safe stack.
    return true;
  }
  
  //Paul: read from the file and strore the info in the direct parrents or
  //direct parents phantom
  void HexTypeLLVMUtil::getTypeInfoFromClang() {
    if (getenv("HEXTYPE_LOG_PATH") != nullptr) {
      char path[MAXLEN];
      strcpy(path, getenv("HEXTYPE_LOG_PATH"));
      strcat(path, "/typeinfo.txt");
      FILE *op = fopen(path, "r");
      if(op != nullptr) {
        uint64_t FromTy, ToTy;
        int Type;
        while(fscanf(op, "%d %" PRIu64 "%" PRIu64 "", &Type, &FromTy, &ToTy) != EOF) {
          for (uint32_t i=0;i<AllTypeNum;i++)
            if (AllTypeInfo[i].DetailInfo.TypeHashValue == FromTy) {
              TypeDetailInfo AddTypeInfo;
              AddTypeInfo.TypeHashValue = ToTy;
              AddTypeInfo.TypeIndex = 0;
              if (Type == 1)
                AllTypeInfo[i].DirectParents.push_back(AddTypeInfo);
              if (Type == 2)
                AllTypeInfo[i].DirectPhantomTypes.push_back(AddTypeInfo);
            }
        }
        fclose(op);
      }
    }
  }
  
  //Paul: ?
  void HexTypeLLVMUtil::getDirectTypeInfo(Module &M) {
    std::vector<StructType*> Types = M.getIdentifiedStructTypes();
    for (StructType *ST : Types) {
      TypeInfo NewType;
      //Paul: not interesting, skip
      if (!HexTypeCommonUtil::isInterestingStructType(ST))
        continue;
      
      //Paul: not interesting, skip
      if (!ST->getName().startswith("trackedtype.") ||
          ST->getName().endswith(".base"))
        continue;
      
      //Paul: not interesting, skip
      if (ST->getName().startswith("struct.VerifyResultCache") ||
          ST->getName().startswith("struct.ObjTypeMap"))
        continue;

      parsingTypeInfo(ST, NewType, AllTypeNum++);
      AllTypeInfo.push_back(NewType);
    }
    
    //Paul: read the direct parent and phantom type information from clang
    //the info is read in from a text file, metadata is not passed over metadata nodes
    getTypeInfoFromClang();
    
    //Paul: our stuff
    CastSan.getTypeMetadata(M);

    HashStructTypeMappingVec HashStructVec;

    int i = 0;
    for (auto & typeInfo : AllTypeInfo) {
	    HashStructVec.push_back(std::make_pair(typeInfo.DetailInfo.TypeHashValue, typeInfo.StructTy));
	    i++;
    }
    CastSan.extendByStructTypes(HashStructVec);
    //Paul: build our fake virtual tables
    CastSan.buildFakeVTables();
  }

  bool HexTypeLLVMUtil::isInterestingFn(Function *F) {
    if (F->empty() || F->getEntryBlock().empty() ||
        F->getName().startswith("__init_global_object"))
      return false;

    return true;
  }
  
  //Paul: this inserts a remove stack object info function which will call the runtime
  void HexTypeLLVMUtil::emitRemoveInst(Module *SrcM, IRBuilder<> &BuilderAI,
                                   AllocaInst *TargetAlloca) {
    Value *TypeSize = NULL;
    if (ConstantInt *constantSize =
        dyn_cast<ConstantInt>(TargetAlloca->getArraySize()))
      TypeSize =
        ConstantInt::get(Int64Ty, constantSize->getZExtValue());
    else {
      Value *arraySize = TargetAlloca->getArraySize();
      if (arraySize->getType() != Int64Ty)
        TypeSize = BuilderAI.CreateIntCast(arraySize, Int64Ty, false);
      else
        TypeSize = arraySize;
    }

    StructElementInfoTy Elements;
    Type *AllocaType = TargetAlloca->getAllocatedType();
    getArrayOffsets(AllocaType, Elements, 0);
    if (Elements.size() == 0) return;
    
    //Paul: insert the remove stack object info handler function
    insertRemove(SrcM, BuilderAI, "__remove_stack_oinfo", TargetAlloca,
                 Elements, TypeSize, DL.getTypeAllocSize(AllocaType), NULL);
  }

  //Paul: extends the pahntom types with detailed information, see next function
  void HexTypeLLVMUtil::extendPhantomSet(int TargetIndex, int CurrentIndex) {
    if (VisitCheck[CurrentIndex] == true)
      return;

    VisitCheck[CurrentIndex] = true;

    AllTypeInfo[TargetIndex].AllPhantomTypes.push_back(
      AllTypeInfo[CurrentIndex].DetailInfo);
    TypeInfo* ParentNode = &AllTypeInfo[CurrentIndex];

    for (uint32_t i=0;i<ParentNode->DirectPhantomTypes.size();i++)
      //Paul: recursive call
      extendPhantomSet(TargetIndex, ParentNode->DirectPhantomTypes[i].TypeIndex);
    return;
  }
   
  //Paul: extends the parent set with detailed information
  void HexTypeLLVMUtil::extendParentSet(int TargetIndex, int CurrentIndex) {
    if (VisitCheck[CurrentIndex] == true)
      return;

    VisitCheck[CurrentIndex] = true;
    
    //Paul: add for each parent its parents int the all type info
    AllTypeInfo[TargetIndex].AllParents.push_back(AllTypeInfo[CurrentIndex].DetailInfo);
    TypeInfo* ParentNode = &AllTypeInfo[CurrentIndex];

    for (uint32_t i=0;i<ParentNode->DirectParents.size();i++)
      //Paul: recursice call
      extendParentSet(TargetIndex, ParentNode->DirectParents[i].TypeIndex);

    for (uint32_t i=0;i<ParentNode->DirectPhantomTypes.size();i++)
       //Paul: recursive call
       extendParentSet(TargetIndex, ParentNode->DirectPhantomTypes[i].TypeIndex);
    return;
  }
   
  //Paul: building fake virtual tables
  int HexTypeLLVMUtil::buildFakeVTables(int start, int t, int root) {
    for (int i = 0; i < AllTypeInfo[t].FakeVPointers.size(); i++)
      if (AllTypeInfo[t].FakeVPointers[i].first == root)
        return start;
    AllTypeInfo[t].FakeVPointers.push_back(std::make_pair(root, start));
    start = start + 1;

    if (AllTypeInfo[t].isRoot && root != t)
    {
        for (int i = 0; i < AllTypeInfo[t].DiamondRootInTree.size(); i++)
        {
            if (AllTypeInfo[t].DiamondRootInTree[i] == root)
            {
                return start;
            }
        }
    }

    // TODO: parents from llvm StructType are too many, get precise info from clang!
    for (int i = 0; i < AllTypeInfo[t].DirectChildren.size(); i++)
        start = buildFakeVTables(start, AllTypeInfo[t].DirectChildren[i], root);

    return start;
  }

  //Paul: add all direct parents and all direct children to the AllTypeInfo
  void HexTypeLLVMUtil::extendTypeRelationInfo() {
    for (uint32_t i=0;i<AllTypeNum;i++)
    {
      if (!AllTypeInfo[i].DirectParents.size())
      {
        AllTypeInfo[i].isRoot = true;
      }
      for (uint32_t j=0;j<AllTypeInfo[i].DirectParents.size();j++)
        for (uint32_t t=0;t<AllTypeNum;t++)
          if (i != t &&
              (AllTypeInfo[i].DirectParents[j].TypeHashValue ==
               AllTypeInfo[t].DetailInfo.TypeHashValue)) {
            //Paul: add all direct parents to the all type info
            AllTypeInfo[i].DirectParents[j].TypeIndex = t;

            bool knownChild = false;
            for (int k = 0; k < AllTypeInfo[t].DirectChildren.size(); k++)
              if (AllTypeInfo[t].DirectChildren[k] == i)
                knownChild = true;

            if (!knownChild)
            {
              //Paul: add all direct children to the all type info 
              AllTypeInfo[t].DirectChildren.push_back(i);
            }
            
            //Paul: used by phantom classes
            //if data layout (DL) is the same than we have a phantom class
            if (DL.getTypeAllocSize(AllTypeInfo[i].StructTy) ==
                DL.getTypeAllocSize(AllTypeInfo[t].StructTy)) {
              //Paul: store the same info at index i and t
              //Paul: at index i add all phantom type info from index j
              AllTypeInfo[i].DirectPhantomTypes.push_back(AllTypeInfo[t].DetailInfo);
              //Paul: at index t add all phantom type info from index i
              AllTypeInfo[t].DirectPhantomTypes.push_back(AllTypeInfo[i].DetailInfo);
            }
          }
    }
    
    for (uint32_t i=0;i<AllTypeNum;i++) {
      std::fill_n(VisitCheck, AllTypeNum, false);
      extendParentSet(i, i);

      std::fill_n(VisitCheck, AllTypeNum, false);
      extendPhantomSet(i, i);
    }
  }
  
  //Paul: sorting a set
  void HexTypeLLVMUtil::sortSet(std::set<uint64_t> &TargetSet) {
    std::vector<uint64_t> tmpSort;
    for (std::set<uint64_t>::iterator it=TargetSet.begin();
         it!=TargetSet.end(); ++it)
      tmpSort.push_back(*it);
    //Paul: sort this set, this is a basic sort function
    sort(tmpSort.begin(), tmpSort.end());

    TargetSet.clear();
    for(size_t i=0; i<tmpSort.size(); i++)
      TargetSet.insert(tmpSort[i]);
  }
  
  //Paul: sort parent set
  void HexTypeLLVMUtil::getSortedAllParentSet() {
    typeInfoArray.push_back(ConstantInt::get(Int64Ty,
                                             AllTypeNum));
    typeInfoArrayInt.push_back(AllTypeNum);
    for (uint32_t i=0;i<AllTypeNum;i++) {
      typeInfoArray.push_back(
        ConstantInt::get(Int64Ty,
                         AllTypeInfo[i].DetailInfo.TypeHashValue));

      typeInfoArrayInt.push_back(AllTypeInfo[i].DetailInfo.TypeHashValue);

      std::set<uint64_t> TmpSet;
      for (unsigned long j=0;j<AllTypeInfo[i].AllParents.size();j++)
        TmpSet.insert(AllTypeInfo[i].AllParents[j].TypeHashValue);
      //Paul: see previous function 
      sortSet(TmpSet);
    
      typeInfoArray.push_back(
        ConstantInt::get(Int64Ty, TmpSet.size()));
      //Paul: store the size of the set
      typeInfoArrayInt.push_back(TmpSet.size());

      for (std::set<uint64_t>::iterator it=TmpSet.begin();
           it!=TmpSet.end(); ++it) {
        typeInfoArray.push_back(ConstantInt::get(Int64Ty, *it));
        typeInfoArrayInt.push_back(*it);
      }
      //Paul: clear temporary set
      TmpSet.clear();
    }
  }
  
  //Paul: sort phantom set
  void HexTypeLLVMUtil::getSortedAllPhantomSet() {
    int phantomTypeCnt = 0;
    for (uint32_t i=0;i<AllTypeNum;i++)
      if (AllTypeInfo[i].AllPhantomTypes.size() > 1)
        phantomTypeCnt +=1;
    typePhantomInfoArray.push_back(
      ConstantInt::get(Int64Ty, phantomTypeCnt));

    for (uint32_t i=0;i<AllTypeNum;i++) {
      if (AllTypeInfo[i].AllPhantomTypes.size() <= 1)
        continue;

      typePhantomInfoArray.push_back(
        ConstantInt::get(Int64Ty,
                         AllTypeInfo[i].DetailInfo.TypeHashValue));

      std::set<uint64_t> TmpOnlyPhantomSet;
      for (unsigned long j=0;j<AllTypeInfo[i].AllPhantomTypes.size();j++)
        TmpOnlyPhantomSet.insert(
          AllTypeInfo[i].AllPhantomTypes[j].TypeHashValue);
      //Paul: sort the phantom set
      sortSet(TmpOnlyPhantomSet);
      typePhantomInfoArray.push_back(
        ConstantInt::get(Int64Ty, TmpOnlyPhantomSet.size()));

      for (std::set<uint64_t>::iterator it=TmpOnlyPhantomSet.begin();
           it!=TmpOnlyPhantomSet.end(); ++it)
        typePhantomInfoArray.push_back(
          ConstantInt::get(Int64Ty, *it));

      TmpOnlyPhantomSet.clear();
    }
  }
  
  //Paul: set casting related set
  void HexTypeLLVMUtil::setCastingRelatedSet() {
    if (getenv("HEXTYPE_LOG_PATH") != nullptr) {
      char path[MAXLEN];
      strcpy(path, getenv("HEXTYPE_LOG_PATH"));
      strcat(path, "/casting_obj.txt");

      FILE *op = fopen(path, "r");
      if(op != nullptr) {
        char TypeName[MAXLEN];
        flockfile(op);
        //Paul: read from the file
        while(fscanf(op, "%s", TypeName) == 1) {
          std::string upTypeName(TypeName);
          //Paul: fill the casting related set from file
          CastingRelatedSet.insert(upTypeName);
        }
        fclose(op);
        funlockfile(op);
      }
    }
  }

  void HexTypeLLVMUtil::extendCastingRelatedTypeSet() {
    for (uint32_t i=0;i<AllTypeNum;i++)
      for (unsigned long j=0;j<AllTypeInfo[i].AllParents.size();j++)
        if ((CastingRelatedSet.find(AllTypeInfo[i].AllParents[j].TypeName) !=
             CastingRelatedSet.end())) {
          CastingRelatedExtendSet.insert(AllTypeInfo[i].DetailInfo.TypeName);
          break;
        }

    std::set<std::string>::iterator it;
    for (it = CastingRelatedExtendSet.begin();
         it != CastingRelatedExtendSet.end(); ++it) {
      if((CastingRelatedSet.find(*it)) == CastingRelatedSet.end()) {
        char fileName[MAXLEN];
        char fileNameTmp[MAXLEN];
        strcpy(fileName, "/casting_obj");
        sprintf(fileNameTmp,"_%d.txt", getpid());
        strcat(fileName, fileNameTmp);

        char tmp[MAXLEN];
        sprintf(tmp, "%s", (*it).c_str());
        writeInfoToFile(tmp, fileName);
        CastingRelatedSet.insert((*it));
      }
    }
  }
  
  //Paul: build relation info, all parent sets and all phatom sets
  void HexTypeLLVMUtil::createObjRelationInfo(Module &M) {
    getDirectTypeInfo(M);
    if (AllTypeInfo.size() == 0)
      return;
    //Paul: add all direct parents and direct children to the alltypemap 
    extendTypeRelationInfo();
    //Paul: sort the parent set, just simple sorting
    getSortedAllParentSet();
    //Paul: sort the phantom set, just simple sorting
    getSortedAllPhantomSet();
  }
  
  //Paul: init some local types
  void HexTypeLLVMUtil::initType(Module &M) {
    LLVMContext& Ctx = M.getContext();

    VoidTy = Type::getVoidTy(Ctx);
    Int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(Ctx));
    Int32PtrTy = PointerType::getUnqual(Type::getInt32Ty(Ctx));
    Int64PtrTy = PointerType::getUnqual(Type::getInt64Ty(Ctx));
    IntptrTy = Type::getInt8PtrTy(Ctx);
    IntptrTyN = DL.getIntPtrType(Ctx);
    Int128Ty = Type::getInt128Ty(Ctx);
    Int64Ty = Type::getInt64Ty(Ctx);
    Int32Ty = Type::getInt32Ty(Ctx);
    Int8Ty = Type::getInt8Ty(Ctx);
    Int1Ty = Type::getInt1Ty(Ctx);
  }
   
  //Paul: print into a txt file
  void HexTypeCommonUtil::writeInfoToFile(char *Info, char *FilePath) {
    assert(Info && "Invalid information");
    assert(FilePath && "Invalid filepath");
    if ((Info != NULL) && getenv("HEXTYPE_LOG_PATH") != nullptr) {
      char path[MAXLEN];
      strcpy(path, getenv("HEXTYPE_LOG_PATH"));
      strcat(path, FilePath);

      FILE *op = fopen(path, "a");
      if (op) {
        fprintf(op, "%s\n", Info);
        fflush(op);
        fclose(op);
      }
    }
  }
  
  //Paul: see above function
  void HexTypeCommonUtil::updateCastingReleatedTypeIntoFile(Type *SrcTy) {
    if(SrcTy->isPointerTy()) {
      llvm::PointerType *ptr = cast<llvm::PointerType>(SrcTy);
      if(llvm::Type *AggTy = ptr->getElementType())
        if(AggTy->isStructTy()) {
          char fileName[MAXLEN];
          char fileNameTmp[MAXLEN];
          strcpy(fileName, "/casting_obj_init");
          sprintf(fileNameTmp,"_%d.txt", getpid());
          strcat(fileName, fileNameTmp);

          char tmp[MAXLEN];
          std::string TypeName(AggTy->getStructName().str());
          syncTypeName(TypeName);
          sprintf(tmp, "%s", TypeName.c_str());
          writeInfoToFile(tmp, fileName);
        }
    }
  }
  
  //Paul: used to remove some substrings from other strings
  void HexTypeCommonUtil::syncTypeName(std::string& TargetStr) {
    SmallVector<std::string, 12> RemoveStrs;
    RemoveStrs.push_back("class.");
    RemoveStrs.push_back("./");
    RemoveStrs.push_back("struct.");
    RemoveStrs.push_back("union.");
    RemoveStrs.push_back(".base");
    RemoveStrs.push_back("trackedtype.");
    RemoveStrs.push_back("blacklistedtype.");
    RemoveStrs.push_back("*");
    RemoveStrs.push_back("'");

    for (unsigned long i=0; i<RemoveStrs.size(); i++)
      //Paul: remove the above stings from all target string
      removeTargetStr(TargetStr, RemoveStrs[i]);
    removeTargetNum(TargetStr);
  }
  
  //Paul: get next instruction
  Instruction* HexTypeLLVMUtil::findNextInstruction(Instruction *CurInst) {
    BasicBlock::iterator it(CurInst);
    ++it;
    if (it == CurInst->getParent()->end())
      return NULL;

    return &*it;
  }
  
  //Paul: provides the offsets of the struct type 
  void HexTypeLLVMUtil::getArrayOffsets(Type *AI, StructElementInfoTy &Elements,
                                    uint32_t Offset) {
    if (ArrayType *Array = dyn_cast<ArrayType>(AI)) {
      uint32_t ArraySize = Array->getNumElements();
      //Paul: this is an element of the initial array type
      Type *AllocaType = Array->getElementType();
      for (uint32_t i = 0; i < ArraySize ; i++) {
        //Paul: recursive call
        getArrayOffsets(AllocaType, Elements,
                        (Offset + (i * DL.getTypeAllocSize(AllocaType))));
      }
    }
    
    //Paul: this breaks each array type into an struct type
    // at this momment it can retrieve the offsets and put them into the elements array.
    else if (StructType *STy = dyn_cast<StructType>(AI))
      if (isInterestingStructType(STy))
        HexTypeLLVMUtil::getStructOffsets(STy, Elements, Offset);
  }
  
  //Paul: this is called from inside the function above.
  //
  void HexTypeLLVMUtil::getStructOffsets(StructType *STy,
                                     StructElementInfoTy &Elements,
                                     uint32_t Offset) {
    const StructLayout *SL = DL.getStructLayout(STy);
    bool duplicate = false;

    for (std::list<std::pair<uint64_t, StructType*>>::iterator it =
         Elements.begin(); it != Elements.end(); it++)
      if (Offset == it->first) {
        duplicate = true;
        break;
      }

    if (!duplicate)
      if (STy->getName().startswith("trackedtype."))
        //Paul: it populates the elements array
        Elements.push_back(std::make_pair(Offset, STy));

    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
      uint32_t tmp = SL->getElementOffset(i) + Offset;
      HexTypeLLVMUtil::getArrayOffsets(STy->getElementType(i), Elements, tmp);
    }
  }
  
  //Paul: remove objects which are not used for casting
  void HexTypeLLVMUtil::removeNonCastingRelatedObj(StructElementInfoTy &Elements) {
    bool FindType = true;
    //Paul: Elements is a work list.
    while (FindType && Elements.size() > 0) {
      FindType = false;
      std::list<std::pair<uint64_t, StructType*>>::iterator it1;
      it1 = Elements.begin();

      for (auto &entry : Elements) {
        assert(entry.second != nullptr);
        assert(entry.second->hasName());

        std::string TargetStr = entry.second->getName();
        syncTypeName(TargetStr);
        //Paul: search in the casting related set for the target str, 
        //in case it was found then it is safe to remove this element
        if ((CastingRelatedSet.find(TargetStr)) == CastingRelatedSet.end()) {
          Elements.erase(it1);
          FindType = true;
          break;
        }
        it1++;
      }
    }
  }
  
  //Paul: verify the result cache
  GlobalVariable *HexTypeLLVMUtil::getVerifyResultCache(Module &M) {
    llvm::SmallString<32> ResultCacheTyName("struct.VerifyResultCache");
    llvm::Type *FieldTypes[] = {
      Int64Ty,
      Int64Ty,
      Int8Ty };
    llvm::StructType *ResultCacheTy =
      llvm::StructType::create(M.getContext(),
                               FieldTypes, ResultCacheTyName);
    PointerType* ResultCacheTyP = PointerType::get(ResultCacheTy, 0);
    GlobalVariable* ResultCache =
      M.getGlobalVariable("VerifyResultCache", true);

    if (!ResultCache) {
      ResultCache =
        new GlobalVariable(M,
                           ResultCacheTyP,
                           false,
                           GlobalValue::ExternalLinkage,
                           0,
                           "VerifyResultCache");
      ResultCache->setAlignment(8);
    }

    return ResultCache;
  }

  GlobalVariable *HexTypeLLVMUtil::getObjTypeMap(Module &M) {
    llvm::SmallString<32> ObjTypeMapName("struct.ObjHaspMap");
    llvm::Type *FieldTypesObj[] = {
      IntptrTyN,
      IntptrTyN,
      Int64Ty,
      Int32Ty,
      Int32Ty,
      IntptrTyN};
    llvm::StructType *ObjTypeMapTy =
      llvm::StructType::create(M.getContext(),
                               FieldTypesObj, ObjTypeMapName);
    PointerType* ObjTypeMapTyP = PointerType::get(ObjTypeMapTy, 0);
    GlobalVariable* GObjTypeMap = M.getGlobalVariable("ObjTypeMap", true);
    if (!GObjTypeMap) {
      GObjTypeMap =
        new GlobalVariable(M,
                           ObjTypeMapTyP,
                           false,
                           GlobalValue::ExternalLinkage,
                           0,
                           "ObjTypeMap");
      GObjTypeMap->setAlignment(8);
    }

    return GObjTypeMap;
  }
  
  //Paul: emit instructions for object tracing
  //Paul: is the main function for emiting object tracing info.
  //this is the biggest function in this file in terms of number of 
  //code lines
  void HexTypeLLVMUtil::emitInstForObjTrace(Module *SrcM, IRBuilder<> &Builder,
                                            StructElementInfoTy &Elements,
                                            uint32_t EmitType,
                                            Value *ObjAddr, Value *ArraySize,
                                            uint32_t TypeSizeInt,
                                            uint32_t CurrArrayIndex,
                                            uint32_t AllocType,
                                            Value *ReallocAddr,
                                            BasicBlock* BasicBlock,
                                            Type * AllocTypeLLVM) {
    if (ClCastObjOpt && (AllocType != PLACEMENTNEW) &&
        (AllocType != REINTERPRET && AllocType != UPCAST)) {
      //Paul: remove all objects which are not relevant for 
      //casting. These do not appear in a casting operation.
      //This one of the optimizations of HexType, does not trace 
      //such objects
      //we need something similar for poly objects which can not be casted into a 
      //static one, we need to implement our own use def analysis.
      removeNonCastingRelatedObj(Elements);
      if (Elements.size() == 0) return;
    }

    if (ObjAddr && ObjAddr->getType()->isPtrOrPtrVectorTy())
      ObjAddr = Builder.CreatePointerCast(ObjAddr, Int64PtrTy);

    if (ReallocAddr && ReallocAddr->getType()->isPtrOrPtrVectorTy())
      ReallocAddr = Builder.CreatePointerCast(ReallocAddr, Int64PtrTy);

    Value *TypeSize = ConstantInt::get(Int32Ty, TypeSizeInt);
    ConstantInt *constantTypeSize = dyn_cast<ConstantInt>(TypeSize);

    bool isFristEntry = true;
    int k = -1;

    std::vector<int> indexArray;
    uint64_t TypeHash;

    StructType * type = dyn_cast_or_null<StructType>(AllocTypeLLVM);
    if (type)
    {
	    for (int i = 0; i < AllTypeNum; i++)
	    {
		    if (AllTypeInfo[i].StructTy == type)
		    {
			    k = i;
                               //Paul: get the type hash, alltype info stores all type information
			    TypeHash = AllTypeInfo[i].DetailInfo.TypeHashValue;
			    break;
		    }
	    }
    }
    else
    {
	  for (auto & entry : Elements)
	  {
		  uint64_t TypeHashValueInt;
		  if (AllocType == PLACEMENTNEW || AllocType == REINTERPRET)
			  TypeHashValueInt = entry.first;
		  else
			  TypeHashValueInt = getHashValueFromSTy(entry.second);
		  
		  for (int i = 0; i < AllTypeNum; i++)
		  {
			  if (AllTypeInfo[i].DetailInfo.TypeHashValue == TypeHashValueInt)
			  {
				  indexArray.push_back(i);
				  k = i;
                                      //Paul: get the type hash
				  TypeHash = AllTypeInfo[i].DetailInfo.TypeHashValue;
				  break;
			  }
		  }
		  break;
	  }
    }
    if (k == -1) {
	    if (type) {
		    std::cerr << "Type is there! : " << type->getName().str() << std::endl;
		    if (type->getName().str().find(".anon.") != std::string::npos)
		    {
			    std::cerr << "Anon type. Stop here" << std::endl;
			    return;
		    }
	    } else {
		    
		    uint64_t TypeHashValueInt;
		    for (auto & entry : Elements)
		    {
			    if (AllocType == PLACEMENTNEW || AllocType == REINTERPRET)
				    TypeHashValueInt = entry.first;
			    else
				    TypeHashValueInt = getHashValueFromSTy(entry.second);
			    
			    
			    std::cerr << "TypeHash is: " << TypeHashValueInt << std::endl;
			    break;
		    }
	    }
	    if (!isInterestingType(type))
		    return;

	    if (type->getName().startswith("union"))
		    return;

	    if (type->getName().startswith("class.anon"))
		    return;
    }
    assert (k != -1 && "Type not found in AllTypeInfo?");

    CHTreeNode & VType = CastSan.Types[TypeHash];

    for (auto &entry : Elements) {
	  uint32_t OffsetInt;
      if (AllocType == PLACEMENTNEW || AllocType == REINTERPRET)
        OffsetInt = 0;
      else
        OffsetInt = entry.first;

      uint64_t TypeHashValueInt;
      if (AllocType == PLACEMENTNEW || AllocType == REINTERPRET)
        TypeHashValueInt = entry.first;
      else
        TypeHashValueInt = getHashValueFromSTy(entry.second);

      uint32_t vpointer;
      vpointer = CastSan.getFakeVPointer(&VType, TypeHashValueInt).second;

      if (vpointer == -1)
      {
	      continue;
      }

      Value *OffsetV = ConstantInt::get(Int32Ty, OffsetInt);
      OffsetInt += (constantTypeSize->getZExtValue() * CurrArrayIndex);
      Value *first = ConstantInt::get(IntptrTyN, OffsetInt);
      Value *second = Builder.CreatePtrToInt(ObjAddr, IntptrTyN);
      Value *NewAddr = Builder.CreateAdd(first, second);
      Value *ObjAddrT = Builder.CreateIntToPtr(NewAddr, IntptrTyN);
      
      Value *FakeVPointer = ConstantInt::get(Int32Ty, vpointer);
      Value *TypeHashValue = ConstantInt::get(Int64Ty, TypeHashValueInt);
      Value *AllocTypeV = ConstantInt::get(Int32Ty, AllocType);
      // apply Inline optimization
      Value *mapIndex;
      Value *isNull, *isEqual;
      Value *TargetIndexAddr;
      Value *TargetIndexAddrValue;
      Value *mapIndex64;

      static GlobalVariable* GObjTypeMap;
      if (ClInlineOpt && (EmitType == CONOBJADD || EmitType == CONOBJDEL) &&
          (AllocType != REINTERPRET && AllocType != GLOBALALLOC)) {
        GObjTypeMap = getObjTypeMap(*SrcM);

        // create hashmap index
        Value *ShVal = Builder.CreateLShr(NewAddr, 3);
        //Paul: see hardcoded map size value
        Value *mapSize = ConstantInt::get(IntptrTyN, 268435455);
        mapIndex = Builder.CreateAnd(ShVal, mapSize);
        mapIndex64 = Builder.CreatePtrToInt(mapIndex, Int64Ty);

        // get value from the ObjTypeMap table using index
        Value* ObjTypeMapInit = Builder.CreateLoad(GObjTypeMap);
        TargetIndexAddr = Builder.CreateGEP(ObjTypeMapInit, mapIndex, "");

        // Load index
        Value* TargetIndexAddrValueAddr =
          Builder.CreateGEP(TargetIndexAddr, {ConstantInt::get(Int32Ty, 0),
                            ConstantInt::get(Int32Ty, 0)}, "");
        TargetIndexAddrValue =
          Builder.CreateLoad(TargetIndexAddrValueAddr);
        isEqual = Builder.CreateICmpEQ(ObjAddrT, TargetIndexAddrValue);
      }

      switch (EmitType) {
      //Paul: emit update for direct obj. info inline
      case CONOBJADD :
        {
          if (ClInlineOpt &&
              AllocType != REINTERPRET &&
              AllocType != GLOBALALLOC) {
            isNull = Builder.CreateIsNull(TargetIndexAddrValue);
            llvm::Value *isNullandEqual = Builder.CreateOr(isNull, isEqual);
            Instruction *InsertPt = &*Builder.GetInsertPoint();
            TerminatorInst *ThenTerm, *ElseTerm;
            SplitBlockAndInsertIfThenElse(isNullandEqual,
                                          InsertPt, &ThenTerm,
                                          &ElseTerm, nullptr);
            // if ObjTypeMap[index] is empty
            Builder.SetInsertPoint(ThenTerm);
            Value* TargetIndexAddrValueAddrT =
              Builder.CreateGEP(TargetIndexAddr, {ConstantInt::get(Int32Ty, 0),
                                ConstantInt::get(Int32Ty, 0)}, "");
            Builder.CreateStore(ObjAddrT, TargetIndexAddrValueAddrT);
            TargetIndexAddrValueAddrT =
              Builder.CreateGEP(TargetIndexAddr, {ConstantInt::get(Int32Ty, 0),
                                ConstantInt::get(Int32Ty, 2)}, "");
            Builder.CreateStore(FakeVPointer, TargetIndexAddrValueAddrT);

            Builder.SetInsertPoint(ElseTerm);
            Function *initFunction =
              (Function*)SrcM->getOrInsertFunction(
                //Paul: this function will call into the compiler-rt
                //used for updating directly the object type information
                "__update_direct_oinfo_inline", VoidTy,
                IntptrTyN, IntptrTyN, Int32Ty, nullptr);
            Value *Param[5] = {ObjAddrT, 
                               mapIndex64, FakeVPointer};
            //Paul: insert the correspinding call
            Builder.CreateCall(initFunction, Param);
            Builder.SetInsertPoint(InsertPt);
          }
          else {
            char TargetFn[MAXLEN];
            if (AllocType == REINTERPRET)
              strcpy(TargetFn, "__handle_reinterpret_cast");
            else
              strcpy(TargetFn, "__update_direct_oinfo");

            Value *Param[2] = {ObjAddrT, FakeVPointer};
            Function *initFunction =
              //Paul: create the function, see above the two possible names
              (Function*)SrcM->getOrInsertFunction(TargetFn,
                                                   VoidTy, IntptrTyN, Int32Ty, nullptr);
            //Paul: insert the correspinding call
            Builder.CreateCall(initFunction, Param);
          }
          if (ClMakeLogInfo) {
            Value *AllocTypeV =
              ConstantInt::get(Int32Ty, AllocType);
            Function *ObjUpdateFunction =
              (Function*)SrcM->getOrInsertFunction(
                "__obj_update_count", VoidTy,
                Int32Ty, Int64Ty, nullptr);
            Value *TmpOne = ConstantInt::get(Int64Ty, 1);
            Value *Param[2] = {AllocTypeV, TmpOne};
            //Paul: calls obj update count in the compiler-rt
            Builder.CreateCall(ObjUpdateFunction, Param);
          }
          break;
        }
      //Paul: add remove obj. info and update object info.
      case VLAOBJADD:
        {
          if (AllocType == REALLOC) {
            Value *third = Builder.CreatePtrToInt(ReallocAddr, IntptrTyN);
            Value *ObjAddrR =
              Builder.CreateIntToPtr(Builder.CreateAdd(first, third),
                                     IntptrTyN);
            if (isFristEntry) {
              Function *initFunction =
                //Paul: remove object info
                (Function*)SrcM->getOrInsertFunction("__remove_oinfo",
                                                     VoidTy, IntptrTyN,
                                                     Int32Ty, Int64Ty,
                                                     Int32Ty, nullptr);
              Value *Param[4] = {ObjAddrR, TypeSize, ArraySize, AllocTypeV};
              Builder.CreateCall(initFunction, Param);
            }
          }
          Function *initFunction =
            //Paul: update object info
            (Function*)SrcM->getOrInsertFunction("__update_oinfo",
                                                 VoidTy, IntptrTyN, Int32Ty,
                                                 Int64Ty, Int32Ty, nullptr);
          //Paul: here we added our FakeVPointer
          Value *ParamVLAADD[4] = {ObjAddrT, TypeSize, ArraySize, FakeVPointer};
          Builder.CreateCall(initFunction, ParamVLAADD);
          //Paul: object update count 
          if (ClMakeLogInfo) {
            Value *AllocTypeV =
              ConstantInt::get(Int32Ty, AllocType);
            Function *ObjUpdateFunction =
              (Function*)SrcM->getOrInsertFunction(
                "__obj_update_count", VoidTy,
                Int32Ty, Int64Ty, nullptr);
            Value *Param[2] = {AllocTypeV, ArraySize};
            //Paul: add one of the calls from the three functions from above
            Builder.CreateCall(ObjUpdateFunction, Param);
          }
          break;
        }
      //Paul: add the obj. delete and object remove count functions.
      case CONOBJDEL:
        {
         if (ClInlineOpt) {
            // Insert if/else statement
            Instruction *InsertPt = &*Builder.GetInsertPoint();
            TerminatorInst *ThenTerm, *ElseTerm;
            SplitBlockAndInsertIfThenElse(isEqual,
                                          InsertPt, &ThenTerm,
                                          &ElseTerm, nullptr);
            // if ObjTypeMap[index].addr is equal
            Builder.SetInsertPoint(ThenTerm);
            Value* TargetIndexAddrValueAddrT =
              Builder.CreateGEP(TargetIndexAddr, {ConstantInt::get(Int32Ty, 0),
                                ConstantInt::get(Int32Ty, 0)}, "");
            Builder.CreateStore(Constant::getNullValue(IntptrTyN),
                                TargetIndexAddrValueAddrT);
            // if ObjTypeMap[index].addr is not equal, try to remove from RBTree
            Builder.SetInsertPoint(ElseTerm);
            Function *initFunction =
              (Function*)SrcM->getOrInsertFunction(
                //Paul: remove direct object info inline, see compiler-rt
                "__remove_direct_oinfo_inline", VoidTy,
                IntptrTyN, Int64Ty, nullptr);
            Value *Param[2] = {ObjAddrT, mapIndex64};
            Builder.CreateCall(initFunction, Param);
            Builder.SetInsertPoint(InsertPt);
          }
          else {
            Function *initFunction =
              (Function*)SrcM->getOrInsertFunction(
                //Paul: remoce direct object info, see compiler-rt
                "__remove_direct_oinfo", VoidTy,
                IntptrTyN, nullptr);
            Value *ParamCONDEL[1] = {ObjAddrT};
            //Paul: creat the corresponding call
            Builder.CreateCall(initFunction, ParamCONDEL);
          }
          if (ClMakeLogInfo) {
            Value *AllocTypeV =
              ConstantInt::get(Int32Ty, AllocType);
            Function *ObjUpdateFunction =
              (Function*)SrcM->getOrInsertFunction(
                //Paul: remove object count
                "__obj_remove_count", VoidTy,
                Int32Ty, Int64Ty, nullptr);
            Value *TmpOne = ConstantInt::get(Int64Ty, 1);
            Value *Param[2] = {AllocTypeV, TmpOne};
            //Paul: create the call
            Builder.CreateCall(ObjUpdateFunction, Param);
          }
          break;
        }
      //Paul: add the remove object info
      case VLAOBJDEL:
        {
          Function *initFunction =
            (Function*)SrcM->getOrInsertFunction("__remove_oinfo",
                                                 VoidTy, IntptrTyN,
                                                 Int32Ty, Int64Ty,
                                                 Int32Ty, nullptr);
          Value *ParamVLADEL[4] = {ObjAddrT, TypeSize, ArraySize, AllocTypeV};
          Builder.CreateCall(initFunction, ParamVLADEL);
          if (ClMakeLogInfo && AllocType == STACKALLOC) {
            Value *AllocTypeV =
              ConstantInt::get(Int32Ty, AllocType);
            Function *ObjUpdateFunction =
              (Function*)SrcM->getOrInsertFunction(
                //Paul: remove obj count
                "__obj_remove_count", VoidTy,
                Int32Ty, Int64Ty, nullptr);
            Value *TmpOne = ConstantInt::get(Int64Ty, 1);
            Value *Param[2] = {AllocTypeV, TmpOne};
            //Paul: create the call
            Builder.CreateCall(ObjUpdateFunction, Param);
          }
          break;
        }
      }
      isFristEntry = false;
    }
  }
  
  //Paul: check which allocation type it is
  uint32_t getAllocType(std::string RuntimeFnName) {
    if (RuntimeFnName.compare("__update_stack_oinfo") == 0 ||
        RuntimeFnName.compare("__remove_stack_oinfo") == 0)
      return STACKALLOC;
    else if (RuntimeFnName.compare("__update_global_oinfo") == 0)
      return GLOBALALLOC;
    else if (RuntimeFnName.compare("__update_heap_oinfo") == 0 ||
             RuntimeFnName.compare("__remove_heap_oinfo") == 0)
      return HEAPALLOC;
    else if (RuntimeFnName.compare("__placement_new_handle") == 0)
      return PLACEMENTNEW;
    else if (RuntimeFnName.compare("__reinterpret_casting_handle") == 0)
      return REINTERPRET;
    else if (RuntimeFnName.compare("__poly_upcasting_handle") == 0)
      return UPCAST;
    else
      return REALLOC;
  }
  
  //Paul: insert the supported object allocations
  //from inside this function, the main object type handling function (insertUpdate) is called, this locatted above the function from above (getAllocType)
  //in total it is called 4 times depending onm the type of allocation
  void HexTypeLLVMUtil::insertUpdate(Module *SrcM, IRBuilder<> &Builder,
                                 std::string RuntimeFnName, Value *ObjAddr,
                                 StructElementInfoTy &Elements,
                                 uint32_t TypeSize, Value *ArraySize,
                                 Value *ReallocAddr, BasicBlock *BasicBlock, Type * AllocTypeLLVM) {
    uint32_t AllocType = getAllocType(RuntimeFnName);
    if (AllocType == REALLOC) 
      //Paul: object tracing when realloc was used
      emitInstForObjTrace(SrcM, Builder, Elements, VLAOBJADD,
                          ObjAddr,
                          ArraySize, TypeSize, 0,
                          AllocType, ReallocAddr, BasicBlock, AllocTypeLLVM);
    else if (AllocType == PLACEMENTNEW || AllocType == REINTERPRET || AllocType == UPCAST)
      //Paul: object tracing when new() or reninterpret_cast() was used
      emitInstForObjTrace(SrcM, Builder, Elements, CONOBJADD,
                          ObjAddr, ArraySize, TypeSize, 0,
                          AllocType, NULL, BasicBlock, AllocTypeLLVM);
    else if (ArraySize && dyn_cast<ConstantInt>(ArraySize) && AllocType != HEAPALLOC) {
      ConstantInt *constantSize = dyn_cast<ConstantInt>(ArraySize);
      for (uint32_t i=0; i<constantSize->getZExtValue(); i++)
        //Paul: trace object when heap alloc was used
        emitInstForObjTrace(SrcM, Builder, Elements, CONOBJADD,
                            ObjAddr, ArraySize, TypeSize, i,
                            AllocType, NULL, BasicBlock, AllocTypeLLVM);
    }
    else
      emitInstForObjTrace(SrcM, Builder, Elements, VLAOBJADD,
                     ObjAddr,
                     ArraySize, TypeSize, 0,
                     AllocType, NULL, BasicBlock, AllocTypeLLVM);
  }
  
  //Paul: insert remove functions
  void HexTypeLLVMUtil::insertRemove(Module *SrcM, IRBuilder<> &Builder,
                                 std::string RuntimeFnName, Value *ObjAddr,
                                 StructElementInfoTy &Elements,
                                 Value *ArraySize, int TypeSize,
                                 BasicBlock *BasicBlock) {
    if (Elements.size() == 0)
      return;

    if (ArraySize == NULL)
      ArraySize = ConstantInt::get(Int64Ty, 1);
    //Paul: retrieves the type of the allocation, realloc, global alloc, heap alloc, etc.
    uint32_t AllocType = getAllocType(RuntimeFnName);
    if (AllocType != HEAPALLOC && dyn_cast<ConstantInt>(ArraySize)) {
      ConstantInt *constantSize = dyn_cast<ConstantInt>(ArraySize);
      for (uint32_t i=0;i<constantSize->getZExtValue();i++)
        //Paul: insert instruction for obj deletion, CONOBJDEL (DEL = delete)
        //this is the main function in this file, it inserts all the runtime calls
        emitInstForObjTrace(SrcM, Builder, Elements, CONOBJDEL,
                            ObjAddr, ArraySize, TypeSize, i, 
                          AllocType, NULL, NULL, NULL);
    }
    else
      //Paul: insert instruction for obj deletion, CONOBJDEL (DEL = delete)
      //in this case the i parameter is 0
      emitInstForObjTrace(SrcM, Builder, Elements, VLAOBJDEL,
                          ObjAddr, ArraySize, TypeSize, 0,
                          AllocType, NULL, NULL, NULL);
  }

  //Paul: strig manipulation function
  void HexTypeLLVMUtil::syncModuleName(std::string& TargetStr) {
    SmallVector<std::string, 12> RemoveStrs;
    RemoveStrs.push_back("./");
    RemoveStrs.push_back(".");
    RemoveStrs.push_back("/");

    for (unsigned long i=0; i<RemoveStrs.size(); i++)
      removeTargetStr(TargetStr, RemoveStrs[i]);

    removeTargetNum(TargetStr);
  }
  
  //Paul: get the hash value from string
  uint64_t HexTypeCommonUtil::getHashValueFromStr(std::string& str) {
    syncTypeName(str);
    unsigned char *className = new unsigned char[str.length() + 1];
    strcpy((char *)className, str.c_str());
    return crc64c(className);
  }
  
  //Paul: get hash value from struct type
  uint64_t HexTypeCommonUtil::getHashValueFromSTy(StructType *STy) {
    std::string str = STy->getName().str();
    syncTypeName(str);
    return getHashValueFromStr(str);
  }
  
  //Paul: just check that the struct type is a struct type, is not literal and 
  //not opaque and has name
  bool HexTypeCommonUtil::isInterestingStructType(StructType *STy) {
    if (STy->isStructTy() &&
        STy->hasName() &&
        !STy->isLiteral() &&
        !STy->isOpaque())
      return true;

    return false;
  }
  
  //Paul: HexType is interested only in struct type and array type
  //only in this types we can have allocation types for objects.
  static bool isInterestingArrayType(ArrayType *ATy) {
    Type *InnerTy = ATy->getElementType();

    if (StructType *InnerSTy = dyn_cast<StructType>(InnerTy))
      //check that the struct has name, and is a struct, is not literal and 
      //not opaque
      return HexTypeCommonUtil::isInterestingStructType(InnerSTy);

    if (ArrayType *InnerATy = dyn_cast<ArrayType>(InnerTy))
      //recall this function recursively
      return isInterestingArrayType(InnerATy);

    return false;
  }
  
  //Paul: this are the currently supported types by HexType
  bool HexTypeCommonUtil::isInterestingType(Type *rootType) {
    //Paul: currently struct types are supported and array types.
    if (StructType *STy = dyn_cast<StructType>(rootType))
      return isInterestingStructType(STy);

    if (ArrayType *ATy = dyn_cast<ArrayType>(rootType))
      //check that his is an array type
      return isInterestingArrayType(ATy);

    return false;
  }

  AllocaInst* HexTypeLLVMUtil::findAllocaForValue(Value *V) {
    if (AllocaInst *AI = dyn_cast<AllocaInst>(V))
      return AI; // TODO: isInterestingAlloca(*AI) ? AI : nullptr;

    AllocaInst *Res = nullptr;
    if (CastInst *CI = dyn_cast<CastInst>(V))
      Res = findAllocaForValue(CI->getOperand(0));
    else if (PHINode *PN = dyn_cast<PHINode>(V))
      for (Value *IncValue : PN->incoming_values()) {
        if (IncValue == PN) continue;
        AllocaInst *IncValueAI = findAllocaForValue(IncValue);
        if (IncValueAI == nullptr || (Res != nullptr && IncValueAI != Res))
          return nullptr;
        Res = IncValueAI;
      }
    return Res;
  }
  
  //Paul: write to file util
  void HexTypeLLVMUtil::setTypeDetailInfo(StructType *STy,
                                          TypeDetailInfo &TargetDetailInfo,
                                          uint32_t AllTypeNum) {
    std::string str = STy->getName().str();
    HexTypeCommonUtil::syncTypeName(str);
    TargetDetailInfo.TypeName.assign(str);
    TargetDetailInfo.TypeHashValue =
      HexTypeCommonUtil::getHashValueFromSTy(STy);
    TargetDetailInfo.TypeIndex = AllTypeNum;

    if (ClMakeTypeInfo) {
      char fileName[MAXLEN];
      char tmp[MAXLEN];
      strcpy(fileName, "/typehashinfo.txt");
      sprintf(tmp,"%" PRIu64 " %s", TargetDetailInfo.TypeHashValue, str.c_str());
      writeInfoToFile(tmp, fileName);
    }
    return;
  }
  
  //Paul: printing in the console the parent child relationships
  void HexTypeLLVMUtil::parsingTypeInfo(StructType *STy, TypeInfo &NewType,
                                    uint32_t AllTypeNum) {
    NewType.StructTy = STy;
    NewType.ElementSize = STy->elements().size();
    setTypeDetailInfo(STy, NewType.DetailInfo, AllTypeNum);
    // get parent type information
    if (STy->elements().size() > 0)
      for (StructType::element_iterator I = STy->element_begin(),
           E = STy->element_end(); I != E; ++I) {
        StructType *innerSTy = dyn_cast<StructType>(*I);
        if (innerSTy && isInterestingStructType(innerSTy)) {
          TypeDetailInfo ParentTmp;
          setTypeDetailInfo(innerSTy, ParentTmp, 0);
          NewType.DirectParents.push_back(ParentTmp);
        }
      }

    return;
  }
}
