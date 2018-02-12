#ifndef LLVM_TRANSFORMS_IPO_SAFEDISPATCH_TOOLS_H
#define LLVM_TRANSFORMS_IPO_SAFEDISPATCH_TOOLS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"

#include <string>

#include "SafeDispatchLog.h"

/*Paul:
helper method from the one underneath
*/
static bool sd_isVtableName_ref(const llvm::StringRef& name) {
  if (name.size() <= 4) {
    // name is too short, cannot be a vtable name
    return false;
  } else if (name.startswith("_ZTV") || name.startswith("_ZTC")) {
    llvm::StringRef rest = name.drop_front(4); // drop the _ZT(C|V) part

    return  (! rest.startswith("S")) &&      // but not from std namespace
        (! rest.startswith("N10__cxxabiv")); // or from __cxxabiv
  }

  return false;
}

/*Paul:
this method is used to check if the name is a real v table name.
This method is used when checking that the extracted metadata from 
the Global Variable is a v table.
*/
static bool sd_isVtableName(std::string& className) {
  //just convert the string to a llvm string ref
  llvm::StringRef name(className);

  return sd_isVtableName_ref(name);
}

//Paul: this validates a constant pointer 
//it is only true if start <= off && off < (start + width * 8) evaluates to true 
static bool validConstVptr(llvm::GlobalVariable *rootVtbl, 
                                int64_t start, 
                                int64_t width,
                         const llvm::DataLayout &DL, 
                                     llvm::Value *V, 
                              uint64_t off) { //initial value is 0 

      if (auto GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
        if (GV != rootVtbl)
          return false;

        if (off % 8 != 0)
          return false;
        
        //Paul: this is the only place that the check can get true in this method 
        return start <= off && off < (start + width * 8);
      }

      if (auto GEP = llvm::dyn_cast<llvm::GEPOperator>(V)) {
		llvm::APInt APOffset(DL.getPointerSizeInBits(0), 0);
        bool Result = GEP->accumulateConstantOffset(DL, APOffset);
        if (!Result)
          return false;
        
        //sum up the offset, 
        //getZExtValue() - get the value as a 64-bit unsigned integer after is was zero extended
        //as appropriate for the type of this constant 
        off += APOffset.getZExtValue();
        return validConstVptr(rootVtbl, start, width, DL, GEP->getPointerOperand(), off); //recursive call 
      }
      
      //check the operand type 
      if (auto Op = llvm::dyn_cast<llvm::Operator>(V)) {
        if (Op->getOpcode() == llvm::Instruction::BitCast)//bitcast operation
          return validConstVptr(rootVtbl, start, width, DL, Op->getOperand(0), off);//recursive call

        if (Op->getOpcode() == llvm::Instruction::Select)//select operation
          return validConstVptr(rootVtbl, start, width, DL, Op->getOperand(1), off) &&
                 validConstVptr(rootVtbl, start, width, DL, Op->getOperand(2), off); //two recursive calls 
      }

      return false;
}


#endif

