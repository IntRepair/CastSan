#!/bin/bash

#This script softlinks our modified files into the LLVM source tree

#Path to llvm source tree
llvm=`pwd`/llvm
clang=`pwd`/clang
src=`pwd`/src
runtime=`pwd`/compiler-rt

#llvm include
llvminc=$llvm/include/llvm

#llvm pass
llvmpass=$llvm/lib/Transforms/Instrumentation

#llvm passutil
llvmutil=$llvm/lib/Transforms/Utils

#llvm include
llvminclude=$llvm/include/llvm/Transforms/Utils

#install LLVM codes
rm $llvm/include/llvm/InitializePasses.h
rm $llvm/lib/Analysis/MemoryBuiltins.cpp
rm $llvm/include/llvm/Analysis/MemoryBuiltins.h
rm $llvm/include/llvm/Transforms/Instrumentation.h
rm $llvm/lib/Transforms/Utils/CMakeLists.txt
rm $llvm/lib/Transforms/Instrumentation/CMakeLists.txt
rm $llvmpass/HexTypePass.cpp
rm $llvmpass/HexTypeTreePass.cpp
rm $llvmutil/HexTypeUtil.cpp

cp -f $src/llvm-files/HexTypePass.cpp $llvmpass
cp -f $src/llvm-files/HexTypeTreePass.cpp $llvmpass
cp -f $src/llvm-files/HexTypeUtil.cpp $llvmutil
cp -f $src/llvm-files/HexTypeUtil.h $llvminclude
cp -f $src/llvm-files/InitializePasses.h $llvminc
cp -f $src/llvm-files/MemoryBuiltins.cpp $llvm/lib/Analysis/MemoryBuiltins.cpp
cp -f $src/llvm-files/MemoryBuiltins.h $llvm/include/llvm/Analysis/MemoryBuiltins.h
cp -f $src/llvm-files/Instrumentation.h $llvm/include/llvm/Transforms/Instrumentation.h
cp -f $src/llvm-files/UtilsCMakeLists.txt $llvm/lib/Transforms/Utils/CMakeLists.txt
cp -f $src/llvm-files/InstrumentationCMakeLists.txt $llvm/lib/Transforms/Instrumentation/CMakeLists.txt

#install clang codes
rm $clang/include/clang/Basic/Sanitizers.def
rm $clang/include/clang/Basic/Sanitizers.h
rm $clang/include/clang/Driver/SanitizerArgs.h
rm $clang/lib/CodeGen/BackendUtil.cpp
rm $clang/lib/CodeGen/CGCXXABI.h
rm $clang/lib/CodeGen/CGClass.cpp
rm $clang/lib/CodeGen/CGExpr.cpp
rm $clang/lib/CodeGen/CGExprCXX.cpp
rm $clang/lib/CodeGen/CGExprScalar.cpp
rm $clang/lib/CodeGen/CodeGenFunction.cpp
rm $clang/lib/CodeGen/CodeGenFunction.h
rm $clang/lib/CodeGen/CodeGenTypes.cpp
rm $clang/lib/CodeGen/ItaniumCXXABI.cpp
rm $clang/lib/CodeGen/MicrosoftCXXABI.cpp
rm $clang/lib/Driver/ToolChain.cpp
rm $clang/lib/Driver/ToolChains.cpp
rm $clang/lib/Driver/Tools.cpp
rm $clang/lib/Sema/SemaDecl.cpp
# for clang function test
rm $clang/runtime/CMakeLists.txt
rm $clang/unittests/Frontend/CMakeLists.txt
rm $clang/test/CMakeLists.txt
rm $clang/test/lit.cfg
rm $clang/test/CodeGen/hextype/hextype-dynamic_cast.cpp
rm $clang/test/CodeGen/hextype/hextype-placementnew.cpp
rm $clang/test/CodeGen/hextype/hextype-reinterpret.cpp
rm $clang/test/CodeGen/hextype/hextype-typecasting.cpp

cp -f  $src/clang-files/Sanitizers.def $clang/include/clang/Basic/Sanitizers.def
cp -f  $src/clang-files/Sanitizers.h $clang/include/clang/Basic/Sanitizers.h
cp -f  $src/clang-files/SanitizerArgs.h $clang/include/clang/Driver/SanitizerArgs.h
cp -f  $src/clang-files/BackendUtil.cpp $clang/lib/CodeGen/BackendUtil.cpp
cp -f  $src/clang-files/CGCXXABI.h $clang/lib/CodeGen/CGCXXABI.h
cp -f  $src/clang-files/CGClass.cpp $clang/lib/CodeGen/CGClass.cpp
cp -f  $src/clang-files/CGExpr.cpp $clang/lib/CodeGen/CGExpr.cpp
cp -f  $src/clang-files/CGExprCXX.cpp $clang/lib/CodeGen/CGExprCXX.cpp
cp -f  $src/clang-files/CGExprScalar.cpp $clang/lib/CodeGen/CGExprScalar.cpp
cp -f  $src/clang-files/CodeGenFunction.cpp $clang/lib/CodeGen/CodeGenFunction.cpp
cp -f  $src/clang-files/CodeGenFunction.h $clang/lib/CodeGen/CodeGenFunction.h
cp -f  $src/clang-files/CodeGenTypes.cpp $clang/lib/CodeGen/CodeGenTypes.cpp
cp -f  $src/clang-files/ItaniumCXXABI.cpp $clang/lib/CodeGen/ItaniumCXXABI.cpp
cp -f  $src/clang-files/MicrosoftCXXABI.cpp $clang/lib/CodeGen/MicrosoftCXXABI.cpp 
cp -f  $src/clang-files/ToolChain.cpp $clang/lib/Driver/ToolChain.cpp
cp -f  $src/clang-files/ToolChains.cpp $clang/lib/Driver/ToolChains.cpp
cp -f  $src/clang-files/Tools.cpp $clang/lib/Driver/Tools.cpp
cp -f  $src/clang-files/SemaDecl.cpp $clang/lib/Sema/SemaDecl.cpp
# for clang function test
cp -f  $src/clang-files/test/CMakeLists_runtime.txt $clang/runtime/CMakeLists.txt
cp -f  $src/clang-files/test/CMakeLists_test.txt $clang/test/CMakeLists.txt
cp -f  $src/clang-files/test/CMakeLists_frontend.txt $clang/unittests/Frontend/CMakeLists.txt
cp -f  $src/clang-files/test/lit.cfg $clang/test/lit.cfg
mkdir $clang/test/CodeGen/hextype
cp -f  $src/clang-files/test/hextype-dynamic_cast.cpp $clang/test/CodeGen/hextype/hextype-dynamic_cast.cpp
cp -f  $src/clang-files/test/hextype-placementnew.cpp $clang/test/CodeGen/hextype/hextype-placementnew.cpp
cp -f  $src/clang-files/test/hextype-reinterpret.cpp $clang/test/CodeGen/hextype/hextype-reinterpret.cpp
cp -f  $src/clang-files/test/hextype-typecasting.cpp $clang/test/CodeGen/hextype/hextype-typecasting.cpp

#install compiler-rt codes
rm $runtime/cmake/config-ix.cmake
rm $runtime/lib/CMakeLists.txt
rm $runtime/lib/hextype/CMakeLists.txt
rm $runtime/lib/hextype/hextype.cc
rm $runtime/lib/hextype/hextype.h
rm $runtime/lib/hextype/hextype_rbtree.cc
rm $runtime/lib/hextype/hextype_rbtree.h
rm $runtime/lib/hextype/hextype_report.cc
rm $runtime/lib/hextype/hextype_report.h
rm $runtime/test/CMakeLists.txt
rm $runtime/test/hextype/CMakeLists.txt

cp -f $src/compiler-rt-files/config-ix.cmake $runtime/cmake/config-ix.cmake
cp -f $src/compiler-rt-files/lib_cmakelists.txt $runtime/lib/CMakeLists.txt
mkdir $runtime/lib/hextype
cp -f $src/compiler-rt-files/lib_hextype_cmakelists.txt $runtime/lib/hextype/CMakeLists.txt
cp -f $src/compiler-rt-files/hextype.cc $runtime/lib/hextype/hextype.cc
cp -f $src/compiler-rt-files/hextype.h $runtime/lib/hextype/hextype.h
cp -f $src/compiler-rt-files/hextype_rbtree.cc $runtime/lib/hextype/hextype_rbtree.cc
cp -f $src/compiler-rt-files/hextype_rbtree.h $runtime/lib/hextype/hextype_rbtree.h
cp -f $src/compiler-rt-files/hextype_report.cc $runtime/lib/hextype/hextype_report.cc
cp -f $src/compiler-rt-files/hextype_report.h $runtime/lib/hextype/hextype_report.h
# for compiler-rt function test
mkdir $runtime/test
mkdir $runtime/test/hextype
mkdir $runtime/test/hextype/TestCases
cp -f $src/compiler-rt-files/test/compiler-rt_test_cmakelist.txt $runtime/test/CMakeLists.txt
cp -f $src/compiler-rt-files/test/compiler-rt_test_hextype_cmakelist.txt $runtime/test/hextype/CMakeLists.txt
cp -f $src/compiler-rt-files/test/simple_bad_cast.cc $runtime/test/hextype/TestCases/simple_bad_cast.cc
cp -f $src/compiler-rt-files/test/lit.common.cfg $runtime/test/hextype/lit.common.cfg
cp -f $src/compiler-rt-files/test/lit.site.cfg.in $runtime/test/hextype/lit.site.cfg.in
