//===- CastSanUtil.cpp - helper functions and classes for CastSan ---------===//
////
////                     The LLVM Compiler Infrastructure
////
//// This file is distributed under the University of Illinois Open Source
//// License. See LICENSE.TXT for details.
////
////===--------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/CastSanUtil.h"
#include "llvm/IR/Module.h"
#include <iostream>

namespace llvm {
	void CastSanUtil::getTypeMetadata(Module & M) {
		for (auto MdIt = M.getNamedMDList().begin(); MdIt != M.getNamedMDList().end(); MdIt++) {
			if (!MdIt->getName().startswith(CS_MD_TYPEINFO))
				continue;

			MDString * MangledNameMD = dyn_cast_or_null<MDString>(MdIt->getOperand(0)->getOperand(0));
			assert (MangledNameMD && "CastSan Metadata is missing the mangled name");

			uint64_t TypeHash = getUInt64MD(MdIt->getOperand(1)->getOperand(0));

			CHTreeNode & Type = Types[TypeHash];
			Type.MangledName = MangledNameMD->getString();
			Type.TypeHash = TypeHash;

			uint64_t ParentsNum = getUInt64MD(MdIt->getOperand(2)->getOperand(0));

			std::cerr << "Got MD Type: " << Type.MangledName << " Parents: ";

			for (uint64_t i = 0; i < ParentsNum; i++)
			{
				uint64_t ParentHash = getUInt64MD(MdIt->getOperand(3 + i)->getOperand(0));
				Type.ParentHashes.push_back(ParentHash);

				std::cerr << ParentHash << ", ";
			}

			std::cerr << std::endl;
		}
	}

	uint64_t CastSanUtil::getUInt64MD(const MDOperand & op) {
		ConstantAsMetadata * c = dyn_cast_or_null<ConstantAsMetadata>(op.get());
		assert(c && "Metadata Operand without Metadata");

		ConstantInt * cint = dyn_cast<ConstantInt>(c->getValue());
		assert(cint && "Metdata has no Int Value");

		return cint->getSExtValue();
	}
}
