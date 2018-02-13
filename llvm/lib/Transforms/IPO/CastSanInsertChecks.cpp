/**
 * This Pass will look for cast check metadata intrinsics and replace it with checks
 * The Intrinsics are inserted in the SDUpdateIndices Pass
 */

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
//#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/CallSite.h"
//#include "llvm/IR/MDBuilder.h"

#include "llvm/Transforms/IPO/CastSanLog.h"
#include "llvm/Transforms/IPO/CastSanTools.h"

//#include "llvm/Transforms/Utils/ValueMapper.h"
//#include "llvm/Transforms/Utils/Cloning.h"

#include <list>
#include <vector>
#include <set>
#include <map>
#include <math.h>
#include <algorithm>
#include <iostream>
#include <limits>


using namespace llvm;

struct CastSanInsertChecks : public ModulePass {
	static char ID;

	CastSanInsertChecks() : ModulePass(ID) {
		sd_print("initializing CastSanInsertChecks pass\n");
		initializeCastSanInsertChecksPass(*PassRegistry::getPassRegistry());
	}

	virtual ~CastSanInsertChecks() {
		sd_print("deleting CastSanInsertChecks pass\n");
	}

	bool runOnModule(Module &M) {
		sd_print("\nP5.2 Started running CastSanInsertChecks pass ...\n");
		sd_print("P5.2 Starting final range checks additions for casts ...\n");
      
		/**
		 * Statistics Counters
		 */
		int64_t castConstPtr = 0;
		int64_t castEqSubst = 0;
		int64_t castRangeSubst = 0;
		uint64_t castSumWidth = 0;

		/**
		 * find the subs_cast_check Intrinsics containing the Values for the Range check
		 */
		Function * subst_cast_checkF = M.getFunction(Intrinsic::getName(Intrinsic::subst_cast_check));
      
		const DataLayout &DL = M.getDataLayout();
		LLVMContext& C = M.getContext(); 
		Type *IntPtrTy = DL.getIntPtrType(C, 0);
      
		/**
		 * if the Intrinsic is found replace every use with a check
		 */
		if (subst_cast_checkF) {

			int castRangeCounter = 0;
			int castConstantCounter = 0;

			for (const Use &U : subst_cast_checkF->uses()) {
          
				llvm::CallInst* CI = cast<CallInst>(U.getUser());
				IRBuilder<> builder(CI);

				/**
				 * extract values from the intrinsic
				 */
				llvm::Constant* start        = dyn_cast<Constant>(CI->getArgOperand(0));
				llvm::ConstantInt* width     = dyn_cast<ConstantInt>(CI->getArgOperand(1));
				llvm::ConstantInt* alignment = dyn_cast<ConstantInt>(CI->getArgOperand(2));
				llvm::Value* vptr            = CI->getArgOperand(3);

				/**
				 * we need the values to exist to be able to insert a check
				 */
				assert(vptr && start && width);
          
				int64_t widthInt = width->getSExtValue();
				int64_t alignmentInt = alignment->getSExtValue();
				int alignmentBits = floor(log(alignmentInt + 0.5)/log(2.0));

				llvm::Constant* rootVtblInt    = dyn_cast<llvm::Constant>(start->getOperand(0));
				llvm::GlobalVariable* rootVtbl = dyn_cast<llvm::GlobalVariable>(rootVtblInt->getOperand(0));
				llvm::ConstantInt* startOff    = dyn_cast<llvm::ConstantInt>(start->getOperand(1));
 
				castSumWidth = castSumWidth + widthInt;

				if (validConstVptr(rootVtbl, startOff->getSExtValue(), widthInt, DL, vptr, 0)) {
            
					CI->replaceAllUsesWith(llvm::ConstantInt::getTrue(C));
					CI->eraseFromParent();

					castConstPtr++;
			
				} else if (widthInt > 1) {

					/**
					 * Check if the vptr is between start and start+width. Add Alignment.
					 */
					llvm::Value *vptrInt = builder.CreatePtrToInt(vptr, IntPtrTy);
					llvm::Value *diff = builder.CreateSub(vptrInt, start);
					llvm::Value *diffShr = builder.CreateLShr(diff, alignmentBits);
					llvm::Value *diffShl = builder.CreateShl(diff, DL.getPointerSizeInBits(0) - alignmentBits);
					llvm::Value *diffRor = builder.CreateOr(diffShr, diffShl);
					llvm::Value *inRange = builder.CreateICmpULE(diffRor, width); 
					CI->replaceAllUsesWith(inRange);
					CI->eraseFromParent();
					castRangeSubst += 1;
            
					sd_print("Range: %d has width: % d start: %d vptrInt: %d \n", castRangeSubst, widthInt, start, vptrInt);

				} else {
			  
					llvm::Value *vptrInt = builder.CreatePtrToInt(vptr, IntPtrTy);
					llvm::Value *inRange = builder.CreateICmpEQ(vptrInt, start);

					CI->replaceAllUsesWith(inRange);
					CI->eraseFromParent();
            
					castEqSubst += 1; //count number of equalities substitutions added
				}

			}
		}
		else {
			std::cerr << "CastCheck: no subst_cast_check" << std::endl;
		}
      
		//finished adding all the range checks, now print some statistics.
		//in the interleaving paper the average number of ranges per call site was close to 1 (1,005).
		sd_print("\n P5.2 Finished running CastSandInsertChecks pass...\n");

		sd_print("\n ---P5.2 Cast Subst Statistics--- \n");

		sd_print(" Total range checks added %d \n", castRangeSubst);
		sd_print(" Total eq_checks added %d \n", castEqSubst);
		sd_print(" Total const_ptr % d \n", castConstPtr);
		sd_print(" Average width % lf \n", castSumWidth * 1.0 / (castRangeSubst + castEqSubst + castConstPtr));


		//one of these values has to be > than 0 
		return castRangeSubst > 0 || castEqSubst > 0 || castConstPtr > 0;
	}
};

char CastSanInsertChecks::ID = 0;

ModulePass* llvm::createCastSanInsertChecksPass() {
	return new CastSanInsertChecks();
}

INITIALIZE_PASS(CastSanInsertChecks, "caic", "Module pass for substituting the intrinsics holding a cast metadata by range checks.", false, false)
