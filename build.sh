#!/bin/bash

# get pure llvm source codes
./scripts/get_llvm_src_tree.sh
# install HexType source codes
./scripts/install-hextype-files.sh

if [ -d /home/llvm/source/binutils ]; then
	ln -sf /home/llvm/source/binutils .
fi

rm ./llvm/tools/clang
rm ./llvm/projects/compiler-rt

ln -s ../../clang llvm/tools/clang
ln -s ../../compiler-rt llvm/projects/compiler-rt

# build HexType
case "$(uname -s)" in
  Darwin)
  corecount="$(sysctl -n hw.ncpu)"
  ;;
  *)
  corecount="$(`grep '^processor' /proc/cpuinfo|wc -l`)"
  ;;
esac

make -j"$corecount"
