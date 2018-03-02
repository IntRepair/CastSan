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
  
  //Paul: ?
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
      std::cerr << "New Type: " << ST->getName().data() << std::endl;
      if (!HexTypeCommonUtil::isInterestingStructType(ST))
        continue;

      if (!ST->getName().startswith("trackedtype.") ||
          ST->getName().endswith(".base"))
        continue;

      if (ST->getName().startswith("struct.VerifyResultCache") ||
          ST->getName().startswith("struct.ObjTypeMap"))
        continue;

      parsingTypeInfo(ST, NewType, AllTypeNum++);
      AllTypeInfo.push_back(NewType);
    }

    getTypeInfoFromClang();

    CastSan.getTypeMetadata(M);
    CastSan.buildFakeVTables();
  }

  bool HexTypeLLVMUtil::isInterestingFn(Function *F) {
    if (F->empty() || F->getEntryBlock().empty() ||
        F->getName().startswith("__init_global_object"))
      return false;

    return true;
  }
   
  //Paul: ?
  void HexTypeLLVMUtil::extendPhantomSet(int TargetIndex, int CurrentIndex) {
    if (VisitCheck[CurrentIndex] == true)
      return;

    VisitCheck[CurrentIndex] = true;

    AllTypeInfo[TargetIndex].AllPhantomTypes.push_back(
      AllTypeInfo[CurrentIndex].DetailInfo);
    TypeInfo* ParentNode = &AllTypeInfo[CurrentIndex];

    for (uint32_t i=0;i<ParentNode->DirectPhantomTypes.size();i++)
      extendPhantomSet(TargetIndex,
                       ParentNode->DirectPhantomTypes[i].TypeIndex);
    return;
  }
  
  //Paul: ?
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

    insertRemove(SrcM, BuilderAI, "__remove_stack_oinfo", TargetAlloca,
                 Elements, TypeSize, DL.getTypeAllocSize(AllocaType), NULL);
  }
   
  //Paul: ?
  void HexTypeLLVMUtil::extendParentSet(int TargetIndex, int CurrentIndex) {
    if (VisitCheck[CurrentIndex] == true)
      return;

    VisitCheck[CurrentIndex] = true;

    AllTypeInfo[TargetIndex].AllParents.push_back(
      AllTypeInfo[CurrentIndex].DetailInfo);
    TypeInfo* ParentNode = &AllTypeInfo[CurrentIndex];

    for (uint32_t i=0;i<ParentNode->DirectParents.size();i++)
      extendParentSet(TargetIndex, ParentNode->DirectParents[i].TypeIndex);

    for (uint32_t i=0;i<ParentNode->DirectPhantomTypes.size();i++)
       extendParentSet(TargetIndex,
                     ParentNode->DirectPhantomTypes[i].TypeIndex);
    return;
  }

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

  void HexTypeLLVMUtil::findDiamonds(std::vector<int> & childs, int cur) {
    for (int i = 0; i < AllTypeInfo[cur].DirectChildren.size(); i++)
    {
      for (int k = 0; k < childs.size(); k++)
      {
        if (childs[k] == AllTypeInfo[cur].DirectChildren[i])
        {
          if (!AllTypeInfo[cur].isRoot)
          {
	        std::cerr << std::endl << " -- Diamond Detected: Type " << AllTypeInfo[cur].DetailInfo.TypeName << " is a diamond root in tree " << AllTypeInfo[childs[0]].DetailInfo.TypeName << std::endl;
	        std::cerr << "Childs: ";
	        for (int z = 0; z < childs.size(); z++)
	        {
		        std::cerr << AllTypeInfo[childs[z]].DetailInfo.TypeName << ", ";
	        }
	        std::cerr << std::endl;

	        std::cerr << "Childs of " << AllTypeInfo[cur].DetailInfo.TypeName << ": ";
	        for (int z = 0; z < AllTypeInfo[cur].DirectChildren.size(); z++)
		        std::cerr << AllTypeInfo[AllTypeInfo[cur].DirectChildren[z]].DetailInfo.TypeName << ", ";
	        std::cerr << std::endl;
            AllTypeInfo[cur].isRoot = true;
            AllTypeInfo[cur].DiamondRootInTree.push_back(childs[0]);
            std::vector<int> newChilds = { cur };
            std::cerr << " Finding Diamonds " << AllTypeInfo[cur].DetailInfo.TypeName << ": ";
            findDiamonds(newChilds, cur);
            std::cerr << std::endl;
            return;
          }
        }
      }
      childs.push_back(AllTypeInfo[cur].DirectChildren[i]);
      std::cerr << AllTypeInfo[AllTypeInfo[cur].DirectChildren[i]].DetailInfo.TypeName << ", ";
      findDiamonds(childs, AllTypeInfo[cur].DirectChildren[i]);
    }
  }

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
            AllTypeInfo[i].DirectParents[j].TypeIndex = t;

            bool knownChild = false;
            for (int k = 0; k < AllTypeInfo[t].DirectChildren.size(); k++)
              if (AllTypeInfo[t].DirectChildren[k] == i)
                knownChild = true;

            if (!knownChild)
            {
              AllTypeInfo[t].DirectChildren.push_back(i);
              std::cerr << AllTypeInfo[i].DetailInfo.TypeName << " is a child of " << AllTypeInfo[t].DetailInfo.TypeName << std::endl;
            }

            if (DL.getTypeAllocSize(AllTypeInfo[i].StructTy) ==
                DL.getTypeAllocSize(AllTypeInfo[t].StructTy)) {
              //Paul: store the same info at index i and t
              AllTypeInfo[i].DirectPhantomTypes.push_back(
                AllTypeInfo[t].DetailInfo);
              AllTypeInfo[t].DirectPhantomTypes.push_back(
                AllTypeInfo[i].DetailInfo);
            }
          }
    }

    for (uint32_t i=0;i<AllTypeNum;i++)
    {
      if (AllTypeInfo[i].isRoot)
      {
        std::vector<int> children = { (int)i };
        std::cerr << " Finding Diamonds " << AllTypeInfo[i].DetailInfo.TypeName << ": ";
        findDiamonds(children, i);
        std::cerr << std::endl;
      }
    }
    
    int count = 0;
    for (uint32_t i=0;i<AllTypeNum;i++)
      if (AllTypeInfo[i].isRoot)
      
        count = buildFakeVTables(count, i, i);

    for (uint32_t i=0;i<AllTypeNum;i++) {
      std::fill_n(VisitCheck, AllTypeNum, false);
      extendParentSet(i, i);

      std::fill_n(VisitCheck, AllTypeNum, false);
      extendPhantomSet(i, i);
    }
    
    for (uint32_t i=0;i<AllTypeNum;i++)
    {
        std::cerr << "Type " << AllTypeInfo[i].DetailInfo.TypeName << ": ";
        for (int k = 0; k < AllTypeInfo[i].FakeVPointers.size(); k++)
        {
            std::cerr << "VPointer " << AllTypeInfo[i].FakeVPointers[k].second << " of root " << AllTypeInfo[AllTypeInfo[i].FakeVPointers[k].first].DetailInfo.TypeName << ", ";
        }
        std::cerr << std::endl;
    }
  }
  
  //Paul: sorting a set
  void HexTypeLLVMUtil::sortSet(std::set<uint64_t> &TargetSet) {
    std::vector<uint64_t> tmpSort;
    for (std::set<uint64_t>::iterator it=TargetSet.begin();
         it!=TargetSet.end(); ++it)
      tmpSort.push_back(*it);
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
    extendTypeRelationInfo();
    getSortedAllParentSet();
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
    RemoveStrs.push_back("::");
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
  
  //Paul: ?
  void HexTypeLLVMUtil::getArrayOffsets(Type *AI, StructElementInfoTy &Elements,
                                    uint32_t Offset) {
    if (ArrayType *Array = dyn_cast<ArrayType>(AI)) {
      uint32_t ArraySize = Array->getNumElements();
      Type *AllocaType = Array->getElementType();
      for (uint32_t i = 0; i < ArraySize ; i++) {
        getArrayOffsets(AllocaType, Elements,
                        (Offset + (i * DL.getTypeAllocSize(AllocaType))));
      }
    }

    else if (StructType *STy = dyn_cast<StructType>(AI))
      if (isInterestingStructType(STy))
        HexTypeLLVMUtil::getStructOffsets(STy, Elements, Offset);
  }
  
  //Paul: ?
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
        Elements.push_back(std::make_pair(Offset, STy));

    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
      uint32_t tmp = SL->getElementOffset(i) + Offset;
      HexTypeLLVMUtil::getArrayOffsets(STy->getElementType(i), Elements, tmp);
    }
  }
  
  //Paul: remove objects which are not used for casting
  void HexTypeLLVMUtil::removeNonCastingRelatedObj(StructElementInfoTy &Elements) {
    bool FindType = true;
    while (FindType && Elements.size() > 0) {
      FindType = false;
      std::list<std::pair<uint64_t, StructType*>>::iterator it1;
      it1 = Elements.begin();

      for (auto &entry : Elements) {
        assert(entry.second != nullptr);
        assert(entry.second->hasName());

        std::string TargetStr = entry.second->getName();
        syncTypeName(TargetStr);
        if ((CastingRelatedSet.find(TargetStr)) == CastingRelatedSet.end()) {
          Elements.erase(it1);
          FindType = true;
          break;
        }
        it1++;
      }
    }
  }
  
  //Paul: ?
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
        (AllocType != REINTERPRET)) {
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
				  TypeHash = AllTypeInfo[i].DetailInfo.TypeHashValue;
				  break;
			  }
		  }
		  break;
	  }
    }
    assert (k != -1 && "Type not found in AllTypeInfo?");

    CHTreeNode & VType = CastSan.Types[TypeHash];

    std::cerr << "Insert Update for Type: " << AllTypeInfo[k].DetailInfo.TypeName << std::endl;

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
      vpointer = CastSan.getFakeVPointer(&VType, TypeHashValueInt);

      if (vpointer == -1)
	      continue;

      std::cerr << "Insert Element" << CastSan.Types[TypeHashValueInt].MangledName << " of " << VType.MangledName << ", " << TypeHashValueInt << " with vpointer " << vpointer << std::endl;
	      
      Value *OffsetV = ConstantInt::get(Int32Ty, OffsetInt);
      OffsetInt += (constantTypeSize->getZExtValue() * CurrArrayIndex);
      Value *first = ConstantInt::get(IntptrTyN, OffsetInt);
      Value *second = Builder.CreatePtrToInt(ObjAddr, IntptrTyN);
      Value *NewAddr = Builder.CreateAdd(first, second);
      Value *ObjAddrT = Builder.CreateIntToPtr(NewAddr, IntptrTyN);
      
      Value *FakeVPointer = ConstantInt::get(Int32Ty, vpointer);
      Value *TypeHashValue = ConstantInt::get(Int64Ty, TypeHashValueInt);
      Value *AllocTypeV = ConstantInt::get(Int32Ty, AllocType);
      Value *RuleAddr = nullptr;
      if (EmitType != CONOBJDEL && EmitType != VLAOBJDEL) {
        uint64_t pos = 1;
        for (uint64_t i = 0 ; i < typeInfoArrayInt.at(0); i++) {
          if (typeInfoArrayInt.at(pos++) == TypeHashValueInt)
            break;
          uint64_t interSize = typeInfoArrayInt.at(pos);
          pos += (interSize + 1);
        }
        Value *first = ConstantInt::get(IntptrTyN, (pos * sizeof(uint64_t)));
        Value *second = Builder.CreatePtrToInt(typeInfoArrayGlobal, IntptrTyN);
        RuleAddr = Builder.CreateIntToPtr(Builder.CreateAdd(first, second),
                                          IntptrTyN);
      }

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
      //Paul: ?
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
                                ConstantInt::get(Int32Ty, 1)}, "");
            Builder.CreateStore(RuleAddr, TargetIndexAddrValueAddrT);
            TargetIndexAddrValueAddrT =
              Builder.CreateGEP(TargetIndexAddr, {ConstantInt::get(Int32Ty, 0),
                                ConstantInt::get(Int32Ty, 2)}, "");
            Builder.CreateStore(TypeHashValue, TargetIndexAddrValueAddrT);
            TargetIndexAddrValueAddrT =
              Builder.CreateGEP(TargetIndexAddr, {ConstantInt::get(Int32Ty, 0),
                                ConstantInt::get(Int32Ty, 4)}, "");
            Builder.CreateStore(OffsetV, TargetIndexAddrValueAddrT);
            TargetIndexAddrValueAddrT =
              Builder.CreateGEP(TargetIndexAddr, {ConstantInt::get(Int32Ty, 0),
                                ConstantInt::get(Int32Ty, 5)}, "");
            Builder.CreateStore(FakeVPointer, TargetIndexAddrValueAddrT);

            Builder.SetInsertPoint(ElseTerm);
            Function *initFunction =
              (Function*)SrcM->getOrInsertFunction(
                //Paul: this function will call into the compiler-rt
                "__update_direct_oinfo_inline", VoidTy,
                IntptrTyN, Int64Ty, Int32Ty, IntptrTyN, Int64Ty, Int32Ty, nullptr);
            Value *Param[6] = {ObjAddrT, TypeHashValue, OffsetV,
                               RuleAddr, mapIndex64, FakeVPointer};
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

            Value *Param[5] = {ObjAddrT, TypeHashValue, OffsetV, RuleAddr, FakeVPointer};
            Function *initFunction =
              //Paul: create the function, see above the two possible names
              (Function*)SrcM->getOrInsertFunction(TargetFn,
                                                   VoidTy, IntptrTyN, Int64Ty,
                                                   Int32Ty, IntptrTyN, Int32Ty, nullptr);
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
      //Paul: ? virtual 
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
                                                 VoidTy, IntptrTyN, Int64Ty,
                                                 Int32Ty, Int32Ty,
                                                 Int64Ty, IntptrTyN, Int32Ty, nullptr);
          Value *ParamVLAADD[7] = {ObjAddrT, TypeHashValue, OffsetV,
                                   TypeSize, ArraySize, RuleAddr, FakeVPointer};
          Builder.CreateCall(initFunction, ParamVLAADD);
          //Paul: object updaye count 
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
      //Paul: object delete
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
    else
      return REALLOC;
  }
  
  //Paul: insert the update functions
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
    else if (AllocType == PLACEMENTNEW || AllocType == REINTERPRET)
      //Paul: object tracing when new() or reninterpret_cast() was used
      emitInstForObjTrace(SrcM, Builder, Elements, CONOBJADD,
                          ObjAddr, ArraySize, TypeSize, 0,
                          AllocType, NULL, BasicBlock, AllocTypeLLVM);
    else if (dyn_cast<ConstantInt>(ArraySize) && AllocType != HEAPALLOC) {
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

    uint32_t AllocType = getAllocType(RuntimeFnName);
    if (AllocType != HEAPALLOC && dyn_cast<ConstantInt>(ArraySize)) {
      ConstantInt *constantSize = dyn_cast<ConstantInt>(ArraySize);
      for (uint32_t i=0;i<constantSize->getZExtValue();i++)
        //Paul: insert instruction for obj deletion, CONOBJDEL (DEL = delete)
        emitInstForObjTrace(SrcM, Builder, Elements, CONOBJDEL,
                            ObjAddr, ArraySize,
                            TypeSize, i, AllocType, NULL, NULL, NULL);
    }
    else
      //Paul: insert instruction for obj deletion, CONOBJDEL (DEL = delete)
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
  
  //Paul: ?
  bool HexTypeCommonUtil::isInterestingStructType(StructType *STy) {
    if (STy->isStructTy() &&
        STy->hasName() &&
        !STy->isLiteral() &&
        !STy->isOpaque())
      return true;

    return false;
  }
  
  //Paul: ?
  static bool isInterestingArrayType(ArrayType *ATy) {
    Type *InnerTy = ATy->getElementType();

    if (StructType *InnerSTy = dyn_cast<StructType>(InnerTy))
      return HexTypeCommonUtil::isInterestingStructType(InnerSTy);

    if (ArrayType *InnerATy = dyn_cast<ArrayType>(InnerTy))
      return isInterestingArrayType(InnerATy);

    return false;
  }
  
  //Paul: ?
  bool HexTypeCommonUtil::isInterestingType(Type *rootType) {
    if (StructType *STy = dyn_cast<StructType>(rootType))
      return isInterestingStructType(STy);

    if (ArrayType *ATy = dyn_cast<ArrayType>(rootType))
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
  
  //Paul: ?
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
          std::cerr << "Parsing " << NewType.DetailInfo.TypeName << " is a child of " << ParentTmp.TypeName << std::endl;
          NewType.DirectParents.push_back(ParentTmp);
        }
      }

    return;
  }
}
