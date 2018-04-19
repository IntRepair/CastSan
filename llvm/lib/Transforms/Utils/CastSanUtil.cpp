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
		assert(Types.size() == 0 && "Already have Metadata....");
		for (auto MdIt = M.getNamedMDList().begin(); MdIt != M.getNamedMDList().end(); MdIt++) {
			if (!MdIt->getName().startswith(CS_MD_TYPEINFO))
				continue;

			MDString * MangledNameMD = dyn_cast_or_null<MDString>(MdIt->getOperand(0)->getOperand(0));
			assert (MangledNameMD && "CastSan Metadata is missing the mangled name");

			uint64_t TypeHash = getUInt64MD(MdIt->getOperand(1)->getOperand(0));

			CHTreeNode & Type = Types[TypeHash];
			
			if (Type.TypeHash != 0) {
				if (Type.MangledName != MangledNameMD->getString())
					std::cerr << "Type " << Type.MangledName << " is also known as " << MangledNameMD->getString().str() << std::endl;
			} else {
				auto test = MangledNameMD -> getString();
				Type.MangledName = MangledNameMD->getString();
				Type.TypeHash = TypeHash;
				
				uint64_t Polymorphic = getUInt64MD(MdIt->getOperand(2)->getOperand(0));
				Type.Polymorphic = Polymorphic == 1 ? true : false;
			}
			
			uint64_t ParentsNum = getUInt64MD(MdIt->getOperand(3)->getOperand(0));

			for (uint64_t i = 0; i < ParentsNum; i++)
			{
				uint64_t ParentHash = getUInt64MD(MdIt->getOperand(4 + i)->getOperand(0));
				
				bool alreadyChild = false;
				for (auto node : Type.ParentHashes)
					if (node == ParentHash)
						alreadyChild = true;

				if (!alreadyChild && ParentHash != TypeHash) {
					Type.ParentHashes.push_back(ParentHash);
				}
			}

		}

		// sanity checks. Each Mangled Name should be unique.
		for (auto & node : Types)
		{
			if (node.second.ParentHashes.size() == 0) {
				Roots.push_back(&node.second);
			}
			int count = 0;
			for (auto & innerNode : Types)
			{
				if (node.second.MangledName == innerNode.second.MangledName)
					count++;
			}
			assert (count == 1 && "There is a duplicate Type with different Hashes.");
		}

		extendTypeMetadata();
	}

	void CastSanUtil::PrintTree(CHTreeNode * root, int deep) {
		for (int i = 0; i < deep; i++)
			std::cerr << " ";
		std::cerr << root->MangledName << std::endl;
		for (auto child : root->Children) {
			PrintTree(child, deep + 1);
		}
	}

	void CastSanUtil::extendTypeMetadata() {
		for (auto & Type : Types) // Type: std::pair<uint64_t, CHTreeNode>
		{
			for (auto & ParentHash : Type.second.ParentHashes)
			{
				auto & Parent = Types[ParentHash];
				bool alreadyParent = false;
				for (auto node : Parent.ChildHashes)
					if (node == Type.first)
						alreadyParent = true;

				if (!alreadyParent && &Parent != &Type.second)
				{
					Parent.ChildHashes.push_back(Type.first);
					Type.second.Parents.push_back(&Parent);
				}
				else {
					bool found = false;
					for (auto parent : Type.second.Parents) {
						if (parent == &Parent)
							found = true;
					}
					assert(found && "Already in ChildHashes, but not in Parents?");
				}
			}
		}

        // rerun with leafes to set children in better order (i.e. primary relation are first in order)
		for (auto & Type : Types)
		{
			if (!Type.second.ChildHashes.size())
				setParentsChildrenRecursive(&Type.second);

			for (int i = 0; i < Type.second.Children.size(); i++) {
				for (int k = 0; k < Type.second.Children.size(); k++)
				{
					if (Type.second.Children[i] == Type.second.Children[k])
						assert(i == k && "There are duplicate CHildren....");
				}
			}
		}

		for (auto root : Roots) {
			std::vector<CHTreeNode*> path;
			findLoop(path, root);
		}
	}

	void CastSanUtil::findLoop(std::vector <CHTreeNode*> & path, CHTreeNode * type) {
		std::vector<CHTreeNode*> newPath;
		for (auto node : path) {
			if (node == type) {
				std::cerr << "Loop detected: " << std::endl;
				for (auto n : path) {
					std::cerr << n->MangledName << std::endl;
				}
				std::cerr << type->MangledName << std::endl;
			}
			assert(node != type && "Loop detected.");
			newPath.push_back(node);
		}
		newPath.push_back(type);
		for (auto child : type->Children) {
			findLoop (newPath, child);
		}
	}

	void CastSanUtil::setParentsChildrenRecursive(CHTreeNode * Type)
	{
		for (CHTreeNode * Parent : Type->Parents)
		{
			bool alreadyChild = false;
			for (auto node : Parent->Children)
				if (node == Type)
					alreadyChild = true;

			if (!alreadyChild && Parent != Type) {
				Parent->Children.push_back(Type);
				setParentsChildrenRecursive(Parent);
			}
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
					bool alreadyRoot = false;
					for (auto & node : Roots)
						if (node == Type)
							alreadyRoot = true;
							
					if (!alreadyRoot)
						Roots.push_back(Type);

					bool alreadyDia = false;
					for (auto & dia : Type->DiamondRootInTreeWithChild)
						if (dia.first == Descendents[0] && dia.second == Child)
							alreadyDia = true;
					
					if (!alreadyDia)
						Type->DiamondRootInTreeWithChild.push_back(std::make_pair(Descendents[0], Child));
					return;
				}
			}
			Descendents.push_back(Child);
			findDiamondsRecursive(Descendents, Child);
		}
	}

	bool CastSanUtil::isSubtreeInTree(CHTreeNode * subtree, CHTreeNode * tree, CHTreeNode * root) {
		if (!root)
			root = tree;
			 
		for (auto Child : tree->Children)
		{
			if (Child == subtree)
			{
				assert (Child->Children.size() && "Subtree is node without children.");

				for (auto GrandChild : subtree->Children)
				{
					if (GrandChild->TreeIndices.count(root) && GrandChild->TreeIndices.count(subtree))
					{
						for (auto & node : subtree->DiamondRootInTreeWithChild)
							if (node.first != root || node.second != GrandChild)
								return true;
					}
				}
			} else {
				assert(Child && Child != tree && "Child loop...");
				if (isSubtreeInTree(subtree, Child, root))
					return true;
			}
		}
		return false;
	}

	void removeTreeIndicesForRoot(CHTreeNode * cur, CHTreeNode * root = nullptr) {
		if (!root)
			root = cur;
		
		cur->TreeIndices.erase(root);
		for (auto child : cur->Children)
			removeTreeIndicesForRoot(child, root);
	}

	void CastSanUtil::removeDuplicates() {
		for (int i = 0; i < Roots.size();)
		{
			bool del = false;
			for (int k = 0; k < Roots.size(); k++)
			{
				if (i != k)
				{
					assert(Roots[i] != Roots[k] && "Duplicate Root in CastSan MD");

					if (isSubtreeInTree(Roots[i], Roots[k]))
					{
						del = true;

						removeTreeIndicesForRoot(Roots[i]);
						Roots.erase(Roots.begin() + i);

						break;
					}
				}
			}
			if (!del)
				i++;
		}
	}

	void CastSanUtil::extendByStructTypes(HashStructTypeMappingVec & vec) {
		for (auto & entry : vec) {
			if (Types.count(entry.first))
				Types[entry.first].StructType = entry.second;
		}
	}

	void CastSanUtil::buildFakeVTables() {
		findDiamonds();

		uint64_t Index = 0;
		for (CHTreeNode * Root : Roots)
		{
			Index = buildFakeVTablesRecursive(Root, Index, Root);
		}

		removeDuplicates();
	}

	uint64_t CastSanUtil::buildFakeVTablesRecursive(CHTreeNode * Root, uint64_t Index, CHTreeNode * Type) {
		Type->TreeIndices[Root] = Index;

		Index++;

		for (CHTreeNode * Child : Type->Children)
		{
			int isPhantomChild = 0;
			if (Type->StructType && Child->StructType && DL.getTypeAllocSize(Type->StructType) == DL.getTypeAllocSize(Child->StructType))
				isPhantomChild = 1;
			for (auto & Node : Type->DiamondRootInTreeWithChild)
				if (Node.first == Root && Node.second == Child)
					continue; // stop here because of diamond

			if (!Type->DiamondRootInTreeWithChild.size())
				Index = buildFakeVTablesRecursive(Root, Index, Child);
			else if (Type == Root)
			{
				for (auto & Node : Type->DiamondRootInTreeWithChild)
					if (Node.second == Child)
						Index = buildFakeVTablesRecursive(Root, Index - isPhantomChild, Child);
			}
			else
			{
				bool isDiamondInheritance = false;
				for (auto & Node : Type->DiamondRootInTreeWithChild)
					if (Node.first == Root && Node.second == Child)
						isDiamondInheritance = true;

				if (!isDiamondInheritance)
					Index = buildFakeVTablesRecursive(Root, Index - isPhantomChild, Child);
			}
		}

		return Index;
	}

	uint64_t CastSanUtil::getUInt64MD(const MDOperand & op) {
		ConstantAsMetadata * c = dyn_cast_or_null<ConstantAsMetadata>(op.get());
		assert(c && "Metadata Operand without Metadata");

		ConstantInt * cint = dyn_cast<ConstantInt>(c->getValue());
		assert(cint && "Metdata has no Int Value");

		return cint->getSExtValue();
	}

	uint64_t CastSanUtil::getRangeWidth(CHTreeNode * Start, CHTreeNode * Root) {
		uint64_t width = 1;

		for (auto Child : Start->Children) {
			for (auto & Node : Start->DiamondRootInTreeWithChild)
				if (Node.first == Root && Node.second == Child)
					continue;

			bool isInRoot = false;
			for (auto & Index : Child->TreeIndices)
				if (Index.first == Root)
					isInRoot = true;

			if (isInRoot)
			{
				width += getRangeWidth(Child, Root);

				// Phantom classes do not increase width
				if (Start->StructType && Child->StructType && DL.getTypeAllocSize(Start->StructType) == DL.getTypeAllocSize(Child->StructType))
					width--;
			}
		}
		return width;
	}

	CHTreeNode * CastSanUtil::getRootForCast(CHTreeNode * P, CHTreeNode * C, CHTreeNode * Root) {
		if (!Root) {
			for (CHTreeNode::TreeIndex & Index : C->TreeIndices) {
				CHTreeNode * RootTry = getRootForCast(P, C, Index.first);
				if (RootTry)
					return RootTry;
			}

			return nullptr;
		}

		for (CHTreeNode * Parent : C->Parents) {
			bool isDiamondParent = false;
			for (auto & Node : Parent->DiamondRootInTreeWithChild)
				if (Node.first == Root && Node.second == C)
					isDiamondParent = true;

			if (isDiamondParent)
				continue;
			
			for (CHTreeNode::TreeIndex & Index : Parent->TreeIndices)
			{
				if (Index.first == Root)
				{
					if (Parent == P)
						return Root;
					CHTreeNode * ParentRoot = getRootForCast(P, Parent, Root);
					if (ParentRoot)
						return ParentRoot;
				}
			}
		}

		return nullptr;
	}

	std::pair<CHTreeNode*,uint64_t> CastSanUtil::getFakeVPointer(CHTreeNode * Type, uint64_t ElementHash)
	{
		CHTreeNode * Root = nullptr;
		if (Type->TypeHash == ElementHash)
		{
			Root = Type;
			while (Root->Parents.size())
				Root = Root->Parents[0];

		}
		else
		{
			for (CHTreeNode * Parent : Type->Parents)
				if (Parent->TypeHash == ElementHash)
				{
					Root = Parent;
					break;
				}

			if (!Root)
				for (CHTreeNode * Parent : Type->Parents)
				{
					auto pvp = getFakeVPointer(Parent, ElementHash);
					if (pvp.first)
					{
						Root = pvp.first;
						break;
					}
				}
		}
		    
		if (Root)
			for (CHTreeNode::TreeIndex & Index : Type->TreeIndices)
				if (Index.first == Root)
					return std::make_pair(Root, Index.second);

		return std::make_pair(nullptr, -1);
	}
}
