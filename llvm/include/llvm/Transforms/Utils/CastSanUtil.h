//===- CastSanUtil.h - helper functions and classes for CastSan ----*- C++-*-===//
////
////                     The LLVM Compiler Infrastructure
////
//// This file is distributed under the University of Illinois Open Source
//// License. See LICENSE.TXT for details.
////
////===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_CASTSAN_H
#define LLVM_TRANSFORMS_UTILS_CASTSAN_H

#include "llvm/IR/IRBuilder.h"
#include <map>
#define CS_MD_TYPEINFO "CS_Type_MD_"


namespace llvm {
	class CHTreeNode {
	public:
		typedef std::pair<CHTreeNode*, uint64_t> TreeIndex;
		
		std::string MangledName;
		uint64_t TypeHash;
		
		std::vector<uint64_t> ParentHashes;
		std::vector<uint64_t> ChildHashes;
		std::vector<CHTreeNode*> Parents;
		std::vector<CHTreeNode*> Children;
		std::vector<TreeIndex> TreeIndices; // FakeVPointer
		std::vector<CHTreeNode*> DiamondRootInTree; // All roots in which this CHTreeNode causes diamond inheritance
	};
	
	class CastSanUtil {
	private:
		void extendTypeMetadata();
		void findDiamonds();
		void findDiamondsRecursive(std::vector<CHTreeNode*> & descendents, CHTreeNode * Type);
		void setParentsChildrenRecursive(CHTreeNode * Type);
		uint64_t buildFakeVTablesRecursive(CHTreeNode * Root, uint64_t Index, CHTreeNode * Type);
	public:
		std::map<uint64_t, CHTreeNode> Types;
		std::vector<CHTreeNode*> Roots;

		void getTypeMetadata(Module & M);
		void buildFakeVTables();

		static uint64_t getUInt64MD(const MDOperand & op);
	};
}

#endif
