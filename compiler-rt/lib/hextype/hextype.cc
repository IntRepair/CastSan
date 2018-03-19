//===-- hextype.cc -- runtime support for HexType  ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===-------------------------------------------------------------------===//

//Paul: one of the main files of hextype, all runtime functionality
//is here contained.
#include "hextype.h"
#include <string.h>
#include <cmath>

//Paul: find an object into the object tyoe map by providing the source address 
//of the object.
__attribute__((always_inline))
  inline ObjTypeMapEntry *findObjInfo(uptr* SrcAddr) {
    uint32_t MapIndex = getHash((uptr)SrcAddr);
    if (ObjTypeMap[MapIndex].ObjAddr == SrcAddr) {
#ifdef HEX_LOG
      IncVal(numLookHit, 1);
#endif
      return &ObjTypeMap[MapIndex];
    }

    if (ObjTypeMap[MapIndex].HexTree != nullptr &&
        ObjTypeMap[MapIndex].HexTree->root != nullptr) {
      ObjTypeMapEntry *FindValue =
        //search in the rb tree for the object
        (ObjTypeMapEntry *)rbtree_lookup(ObjTypeMap[MapIndex].HexTree, SrcAddr);
#ifdef HEX_LOG
      if (FindValue != nullptr)
        IncVal(numLookMiss, 1);
      else
        IncVal(numLookFail, 1);
#endif
      return FindValue;
    }
#ifdef HEX_LOG
    IncVal(numLookFail, 1);
#endif
    return nullptr;
  }

//Paul: this is CastSan function
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
bool __type_casting_verification_ranged(const uint64_t start,
                                        const uint64_t width,
                                        const uint64_t alignment,
                                        const uint64_t alignment_r,
                                        const void* vpointer) {
#ifdef HEX_LOG
    IncVal(numCasting, 1);
    IncVal(numPolyCasting, 1);
#endif
	uint64_t vptr = (uint64_t) vpointer;
	int64_t diff_signed = vptr - start;
	uint64_t diff = *reinterpret_cast<uint64_t*>(&diff_signed);

	uint64_t diffshr = diff >> alignment;
	uint64_t diffshl = diff << alignment_r;

	uint64_t diffRor = diffshr | diffshl;
	if (diffRor < width) {
#ifdef HEX_LOG
		IncVal(numCastNonBadCast, 1);
#endif
		return true;
	}
#ifdef HEX_LOG
	IncVal(numCastBadCast, 1);
#endif
#if defined(PRINT_BAD_CASTING) || defined(PRINT_BAD_CASTING_FILE)
	printTypeConfusion(1, 0, start);
#endif
	return false;
}

//Paul: this yet another CastSan checking function
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
uint8_t __type_casting_verification_equal(const uint64_t start,
                                       const void* vpointer) {
#ifdef HEX_LOG
    IncVal(numCasting, 1);
    IncVal(numPolyCasting, 1);
#endif

	uint64_t vptr = (uint64_t) vpointer;
	if (vptr == start)
	{
		
#ifdef HEX_LOG
		IncVal(numCastSame, 1);
#endif
		return 1;
	}
	
#ifdef HEX_LOG
	IncVal(numCastBadCast, 1);
#endif
#if defined(PRINT_BAD_CASTING) || defined(PRINT_BAD_CASTING_FILE)
	printTypeConfusion(1, 0, start);
#endif
	return 0;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __init_obj_type_map() {
  if (ObjTypeMap == nullptr) {
#ifdef HEX_LOG
    InstallAtExitHandler();
#endif
    ObjTypeMap = new ObjTypeMapEntry[NUMMAP];
  }
}


//Paul: only the first part of this function is relevant for CastSan.
__attribute__((always_inline))
  inline static void verifyTypeCasting(uptr* const SrcAddr,
                                       const uint64_t RangeStart,
                                       const uint64_t RangeWidth) {
	if (SrcAddr == NULL) return;
#ifdef HEX_LOG
    IncVal(numCasting, 1);
    IncVal(numTreeCasting, 1);
#endif
    ObjTypeMapEntry *FindValue = findObjInfo(SrcAddr);
    if (!FindValue)
      return;

#ifdef HEX_LOG
    IncVal(numVerifiedTreeCasting, 1);
#endif
	int64_t diff_signed = FindValue->FakeVPointer - RangeStart;
	uint64_t diff = *reinterpret_cast<uint64_t*>(&diff_signed);

	if (diff >= RangeWidth) {
#ifdef HEX_LOG
		IncVal(numCastBadCast, 1);
#endif
#if defined(PRINT_BAD_CASTING) || defined(PRINT_BAD_CASTING_FILE)
		printTypeConfusion(1, 0, RangeStart);
#endif
		return;
	}
	
#ifdef HEX_LOG
		IncVal(numCastNonBadCast, 1);
#endif
}

//Paul: print the cache results.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __type_casting_verification_print_cache_result(const uint64_t index) {
#ifdef PRINT_BAD_CASTING
  printf("== HexType Type confusion Report\n");
#endif
}

//Paul: this function calls verifyTypeCasting()
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __type_casting_verification_inline_normal(uptr* const SrcAddr,
                                               const uint64_t DstTypeHashValue) {
	verifyTypeCasting(SrcAddr, 0, 0);
}

//Paul: this function calls verifyTypeCasting()
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __type_casting_verification(uptr* const SrcAddr,
                                 const uint64_t DstTypeHashValue,
                                 const uint64_t RangeStart,
                                 const uint64_t RangeWidth) {
	verifyTypeCasting(SrcAddr, RangeStart, RangeWidth);
}

//Paul: this function calls verifyTypeCasting()
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void* __dynamic_casting_verification(uptr* const SrcAddr,
                                     const uint64_t start,
                                     const uint64_t width,
                                     const uint64_t alignment,
                                     const uint64_t alignment_r,
                                     std::ptrdiff_t Src2dst_offset) {
  uptr* TmpAddr = (uptr *)((char *)SrcAddr - Src2dst_offset);
  uint64_t vptr = *(uint64_t*) SrcAddr;
  printf("dyncast!!! vptr: %lu, start: %lu, width: %lu \n", vptr, start, width);
  if (__type_casting_verification_ranged(start, width, alignment, alignment_r, (const void*)vptr))
  {
	  return TmpAddr;
  }
  else
  {
	  return nullptr;
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void* __dynamic_casting_verification_equal(uptr* const SrcAddr,
                                           const uint64_t start,
                                           std::ptrdiff_t Src2dst_offset) {
  uptr* TmpAddr = (uptr *)((char *)SrcAddr - Src2dst_offset);
  uint64_t vptr = *(uint64_t*) SrcAddr;
  printf("dyncast!!! vptr: %lu, start: %lu \n", vptr, start);
  if (__type_casting_verification_equal(start, (const void*)vptr))
	  return TmpAddr;
  else
	  return nullptr;
}

//Paul: update object information
//this function is called when updating the object information directly,
//the function is inserted by the HexTypeTreePass which annotates all relevant locations in code.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __update_direct_oinfo(uptr* const AllocAddr,
                           const uint32_t FakeVPointer) {
  uptr MapIndex = getHash((uptr)AllocAddr);

  printf("Inserting Type info: %lu, %lu, %lu\n", ObjTypeMap, FakeVPointer, AllocAddr);
  if (ObjTypeMap[MapIndex].ObjAddr == nullptr ||
      ObjTypeMap[MapIndex].ObjAddr == AllocAddr) {
    ObjTypeMap[MapIndex].ObjAddr = AllocAddr;
    ObjTypeMap[MapIndex].HeapArraySize = 1;
    ObjTypeMap[MapIndex].FakeVPointer = FakeVPointer;
    return;
  }
#ifdef HEX_LOG
  IncVal(numUpdateMiss, 1);
#endif
  if (ObjTypeMap[MapIndex].HexTree == nullptr)
    //Paul: if there is no tree then a tree is first created.
    ObjTypeMap[MapIndex].HexTree = rbtree_create();

  ObjTypeMapEntry *ObjValue =
    (ObjTypeMapEntry*)malloc(sizeof(ObjTypeMapEntry));
  memcpy(ObjValue, &ObjTypeMap[MapIndex], sizeof(ObjTypeMapEntry));
  
  //Paul: we insert something new in the tree.
  rbtree_insert(ObjTypeMap[MapIndex].HexTree,
                ObjTypeMap[MapIndex].ObjAddr, ObjValue);
  ObjTypeMap[MapIndex].ObjAddr = AllocAddr;
  ObjTypeMap[MapIndex].HeapArraySize = 1;
  ObjTypeMap[MapIndex].FakeVPointer = FakeVPointer;
}

//Paul: update direct object information inlined
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __update_direct_oinfo_inline(uptr* const AllocAddr,
                                  const uint64_t MapIndex, const uint32_t FakeVPointer) {
#ifdef HEX_LOG
  IncVal(numUpdateMiss, 1);
#endif
  if (ObjTypeMap[MapIndex].HexTree == nullptr)
    //Paul: create new rb tree here
    ObjTypeMap[MapIndex].HexTree = rbtree_create();

  ObjTypeMapEntry *ObjValue =
    (ObjTypeMapEntry*)malloc(sizeof(ObjTypeMapEntry));
  memcpy(ObjValue, &ObjTypeMap[MapIndex], sizeof(ObjTypeMapEntry));
  
  //Paul: insert in the rb tree
  rbtree_insert(ObjTypeMap[MapIndex].HexTree,
                ObjTypeMap[MapIndex].ObjAddr, ObjValue);
  ObjTypeMap[MapIndex].ObjAddr = AllocAddr;
  ObjTypeMap[MapIndex].HeapArraySize = 1;
  ObjTypeMap[MapIndex].FakeVPointer = FakeVPointer;
}

//Paul: handle reinterpret cast function
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __handle_reinterpret_cast(uptr* const AllocAddr,
                               const uint32_t FakeVPointer) {
  //Paul: it calls this function located above.
  __update_direct_oinfo(AllocAddr, FakeVPointer);
}

//Paul: update object information.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __update_oinfo(uptr* const AllocAddr,
                    const uint32_t TypeSize, const unsigned long ArraySize,
                    const uint32_t FakeVPointer) {
  for (uint32_t i=0;i<ArraySize;i++) {
    uptr *addr = (uptr *)((char *)AllocAddr + (TypeSize*i));
    uptr MapIndex = getHash((uptr)addr);

    if (ObjTypeMap[MapIndex].ObjAddr == nullptr ||
        ObjTypeMap[MapIndex].ObjAddr == addr) {
      ObjTypeMap[MapIndex].ObjAddr = addr;
      ObjTypeMap[MapIndex].HeapArraySize  = ArraySize;
      ObjTypeMap[MapIndex].FakeVPointer = FakeVPointer;
      continue;
    }

    else {
#ifdef HEX_LOG
      IncVal(numUpdateMiss, 1);
#endif
      if (ObjTypeMap[MapIndex].HexTree == nullptr)
        ObjTypeMap[MapIndex].HexTree = rbtree_create();

      ObjTypeMapEntry *ObjValue =
        (ObjTypeMapEntry*)malloc(sizeof(ObjTypeMapEntry));
      memcpy(ObjValue, &ObjTypeMap[MapIndex], sizeof(ObjTypeMapEntry));

      rbtree_insert(ObjTypeMap[MapIndex].HexTree,
                    ObjTypeMap[MapIndex].ObjAddr, ObjValue);
      ObjTypeMap[MapIndex].ObjAddr = addr;
      ObjTypeMap[MapIndex].HeapArraySize = ArraySize;
      ObjTypeMap[MapIndex].FakeVPointer = FakeVPointer;
    }
  }
}

//Paul: remove object address from object type map
//this function is called when free() is called on an object.
//The object lifetime ends and as such the object can be safely removed
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __remove_direct_oinfo(uptr* const TargetAddr) {
  //Paul: get hash first
  uptr MapIndex = getHash((uptr)TargetAddr);

  if (ObjTypeMap[MapIndex].ObjAddr == TargetAddr) {
    ObjTypeMap[MapIndex].ObjAddr = nullptr;
    return;
  }
#ifdef HEX_LOG
  IncVal(numRemoveMiss, 1);
#endif
  if (ObjTypeMap[MapIndex].HexTree != nullptr &&
      ObjTypeMap[MapIndex].HexTree->root != nullptr) {
    ObjTypeMapEntry* FindValue =
      //Paul: rb tree look-up
      (ObjTypeMapEntry *)rbtree_lookup(ObjTypeMap[MapIndex].HexTree, TargetAddr);
    if (FindValue != nullptr) {
      //Paul: erase the object value
      free(FindValue);
      //Paul: delete the object from the rb tree
      //this helps to keep the per object rb tree as small as possible.
      rbtree_delete(ObjTypeMap[MapIndex].HexTree, TargetAddr);
    }
  }
}

//Paul: same the remove function above. This is the inline function version 
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __remove_direct_oinfo_inline(uptr* const TargetAddr,
                                  const uint64_t MapIndex) {
  if (ObjTypeMap[MapIndex].HexTree != nullptr &&
      ObjTypeMap[MapIndex].HexTree->root != nullptr) {
    ObjTypeMapEntry* FindValue =
      (ObjTypeMapEntry *)rbtree_lookup(ObjTypeMap[MapIndex].HexTree, TargetAddr);
    if (FindValue != nullptr) {
      free(FindValue);
      rbtree_delete(ObjTypeMap[MapIndex].HexTree, TargetAddr);
    }
  }
}

//Paul: another remove version, this removes by checking if it is an heapalloc or an realloc
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __remove_oinfo(uptr* const ObjectAddr, const uint32_t TypeSize,
                    unsigned long ArraySize, const uint32_t AllocType) {
  if (AllocType == HEAPALLOC || AllocType == REALLOC) {
    uptr MapIndex = getHash((uptr)ObjectAddr);
    if (ObjTypeMap[MapIndex].ObjAddr == ObjectAddr)
      ArraySize = ObjTypeMap[MapIndex].HeapArraySize;
    else {
      if (ObjTypeMap[MapIndex].HexTree != nullptr &&
          ObjTypeMap[MapIndex].HexTree->root != nullptr) {
        ObjTypeMapEntry* FindValue =
          (ObjTypeMapEntry *)rbtree_lookup(ObjTypeMap[MapIndex].HexTree, ObjectAddr);
        if (FindValue != nullptr)
          ArraySize = FindValue->HeapArraySize;
      }
      else
        ArraySize = 1;
    }
  }

  for (uint32_t i=0;i<ArraySize;i++) {
    uptr *addr = (uptr *)((char *)ObjectAddr + (TypeSize*i));
    uptr MapIndex = getHash((uptr)addr);
#ifdef HEX_LOG
    switch (AllocType) {
    case HEAPALLOC:
    case REALLOC:
      IncVal(numHeapRm, 1);
      break;
    }
#endif
    if (ObjTypeMap[MapIndex].ObjAddr == addr)
      ObjTypeMap[MapIndex].ObjAddr = nullptr;
    else {
#ifdef HEX_LOG
      IncVal(numRemoveMiss, 1);
#endif
      if (ObjTypeMap[MapIndex].HexTree != nullptr &&
          ObjTypeMap[MapIndex].HexTree->root != nullptr) {
        ObjTypeMapEntry* FindValue =
          //Paul: rb look-up
          (ObjTypeMapEntry *)rbtree_lookup(ObjTypeMap[MapIndex].HexTree, addr);
        if (FindValue != nullptr) {
          //Paul: delete the object
          free(FindValue);
          //Paul: delete the object from the rb tree
          rbtree_delete(ObjTypeMap[MapIndex].HexTree, addr);
        }
      }
    }
  }
}

#ifdef HEX_LOG
//Paul: used for logging
//the function counts if the lookup was successfull.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __lookup_success_count(char VerifyResult) {
  IncVal(numCasting, 1);
  IncVal(numVerifiedCasting, 1);
  IncVal(numLookHit, 1);
  IncVal(numCastHit, 1);

  switch (VerifyResult) {
  case BADCAST:
    IncVal(numCastBadCast, 1);
    break;
  case SAFECASTUPCAST:
    IncVal(numCastNonBadCast, 1);
    break;
  case SAFECASTSAME:
    IncVal(numCastSame, 1);
    break;
  case FAILINFO:
    IncVal(numMissFindObj, 1);
    break;
  }
}

//Paul: count object updates
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __obj_update_count(uint32_t objUpdateType, uint64_t vla) {
  switch (objUpdateType) {
  case STACKALLOC:
    IncVal(numStackUp, vla);
    break;
  case GLOBALALLOC:
    IncVal(numGloUp, vla);
    break;
  case HEAPALLOC: //Paul: todo
  case REALLOC:
    IncVal(numHeapUp, vla);
    break;
  case PLACEMENTNEW:
    IncVal(numplacementNew, vla);
    break;
  case REINTERPRET:
    IncVal(numreinterpretCast, vla);
    break;
  }
}

//Paul: remove count
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void __obj_remove_count(uint32_t objUpdateType, uint64_t vla) {
  switch (objUpdateType) {
  case STACKALLOC: //Paul: only for stacl allocs implemented, why?
    IncVal(numStackRm, vla);
    break;
  }
}
#endif
