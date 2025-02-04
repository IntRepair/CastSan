FROM debian:9-slim

RUN apt update && apt upgrade -y
RUN apt install -y build-essential git bison flex wget python cmake libunwind-dev texinfo git clang llvm

RUN useradd -m -U llvm

WORKDIR /home/llvm
COPY . source

RUN git clone --single-branch -b binutils-2_28-branch https://github.com/bminor/binutils-gdb source/binutils && mkdir -p source/binutils/build

WORKDIR /home/llvm/source/binutils/build
RUN ../configure --enable-gold --enable-plugins --disable-werror && make -j`nproc`

WORKDIR /home/llvm/source/binutils/build/gold
RUN ln -s ld-new ld

WORKDIR /home/llvm/source
RUN ./build.sh