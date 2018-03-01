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

			if (Type.ParentHashes.size())
				continue; // Duplicate Type

			uint64_t ParentsNum = getUInt64MD(MdIt->getOperand(2)->getOperand(0));

			if (!ParentsNum)
			{
				Roots.push_back(&Type);
			}

			std::cerr << "Got MD Type: " << Type.MangledName << " Parents: ";

			for (uint64_t i = 0; i < ParentsNum; i++)
			{
				uint64_t ParentHash = getUInt64MD(MdIt->getOperand(3 + i)->getOperand(0));
				Type.ParentHashes.push_back(ParentHash);

				std::cerr << ParentHash << ", ";
			}

			std::cerr << std::endl;
		}

		extendTypeMetadata();
	}

	void CastSanUtil::extendTypeMetadata() {
		for (auto & Type : Types) // Type: std::pair<uint64_t, CHTreeNode>
		{
			for (auto & ParentHash : Type.second.ParentHashes)
			{
				auto & Parent = Types[ParentHash];
				Parent.ChildHashes.push_back(Type.first);
				Type.second.Parents.push_back(&Parent);
			}
		}

        // rerun with leafes to set children in better order (i.e. primary relation are first in order)
		for (auto & Type : Types)
			if (!Type.second.ChildHashes.size())
				setParentsChildrenRecursive(&Type.second);
	}

	void CastSanUtil::setParentsChildrenRecursive(CHTreeNode * Type)
	{
		for (CHTreeNode * Parent : Type->Parents)
		{
			Parent->Children.push_back(Type);
			setParentsChildrenRecursive(Parent);
		}
	}

	void CastSanUtil::findDiamonds() {
		for (int i = 0; i < Roots.size(); i++)
		{
			std::vector<CHTreeNode*> descendents;
			descendents.push_back(Roots[i]);
			findDiamondsRecursive(descendents, Roots[i]);
		}
	}

	void CastSanUtil::findDiamondsRecursive(std::vector<CHTreeNode*> & Descendents, CHTreeNode * Type) {
		for (CHTreeNode * Child : Type->Children)
		{
			for (CHTreeNode * Desc : Descendents)
			{
				if (Desc->TypeHash == Child->TypeHash)
				{
					Roots.push_back(Type);
					Type->DiamondRootInTree.push_back(Descendents[0]);
					return;
				}
			}
			Descendents.push_back(Child);
			findDiamondsRecursive(Descendents, Child);
		}
	}

	void CastSanUtil::buildFakeVTables() {
		findDiamonds();

		uint64_t Index = 0;
		for (CHTreeNode * Root : Roots)
		{
			std::cerr << std::endl;
			std::cerr << "VTable: " << Root->MangledName << std::endl; 
			Index = buildFakeVTablesRecursive(Root, Index, Root);
		}
	}

	uint64_t CastSanUtil::buildFakeVTablesRecursive(CHTreeNode * Root, uint64_t Index, CHTreeNode * Type) {
		std::cerr << "Entry: " << Type->MangledName << "; Index " << Index << std::endl;
		Type->TreeIndices.push_back(std::make_pair(Root, Index));
		Index++;

		for (CHTreeNode * Node : Type->DiamondRootInTree)
			if (Node == Root)
				return Index; // stop here because of diamond

		for (CHTreeNode * Child : Type->Children)
			Index = buildFakeVTablesRecursive(Root, Index, Child);

		return Index;
	}

	uint64_t CastSanUtil::getUInt64MD(const MDOperand & op) {
		ConstantAsMetadata * c = dyn_cast_or_null<ConstantAsMetadata>(op.get());
		assert(c && "Metadata Operand without Metadata");

		ConstantInt * cint = dyn_cast<ConstantInt>(c->getValue());
		assert(cint && "Metdata has no Int Value");

		return cint->getSExtValue();
	}
}
