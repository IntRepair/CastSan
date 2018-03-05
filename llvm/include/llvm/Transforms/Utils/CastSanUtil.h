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
		typedef std::pair<CHTreeNode* const, uint64_t> TreeIndex;
		
		std::string MangledName;
		uint64_t TypeHash;
		bool Polymorphic;
		
		std::vector<uint64_t> ParentHashes;
		std::vector<uint64_t> ChildHashes;
		std::vector<CHTreeNode*> Parents;
		std::vector<CHTreeNode*> Children;
		std::map<CHTreeNode*, uint64_t> TreeIndices; // FakeVPointer
		std::vector<std::pair<CHTreeNode*, CHTreeNode*>> DiamondRootInTreeWithChild; // All roots in which this CHTreeNode causes diamond inheritance
	};
	
	class CastSanUtil {
	private:
		void extendTypeMetadata();
		void findDiamonds();
		void findDiamondsRecursive(std::vector<CHTreeNode*> & descendents, CHTreeNode * Type);
		void setParentsChildrenRecursive(CHTreeNode * Type);
		uint64_t buildFakeVTablesRecursive(CHTreeNode * Root, uint64_t Index, CHTreeNode * Type);
		bool isSubtreeInTree(CHTreeNode * Subtree, CHTreeNode * Tree, CHTreeNode * Root = nullptr);
		void removeDuplicates();
	public:
		std::map<uint64_t, CHTreeNode> Types;
		std::vector<CHTreeNode*> Roots;

		void getTypeMetadata(Module & M);
		void buildFakeVTables();
		uint64_t getRangeWidth(CHTreeNode * Start, CHTreeNode * Root);
		CHTreeNode * getRootForCast(CHTreeNode * PointerType, CHTreeNode * CastType, CHTreeNode * RootType = nullptr);
		std::pair<CHTreeNode*,uint64_t> getFakeVPointer(CHTreeNode * Type, uint64_t ElementHash);

		static uint64_t getUInt64MD(const MDOperand & op);
	};
}

#endif
