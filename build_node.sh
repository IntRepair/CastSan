#!/bin/bash

NODE_VER="8.10.0"
NODE_TAR="node-v${NODE_VER}.tar.gz"

SETUP_DIR="${PWD}/setup_dir"

NODE_DIR="${SETUP_DIR}/node-v${NODE_VER}"
DOWNLOAD_DIR="${SETUP_DIR}/downloads"

mkdir -p ${DOWNLOAD_DIR}
cd ${SETUP_DIR}

if [ ! -f ${NODE_DIR}/.node_extr ]; then
	[ ! -f ${DOWNLOAD_DIR}/${NODE_TAR} ] && wget -O ${DOWNLOAD_DIR}/${NODE_TAR} https://nodejs.org/dist/v${NODE_VER}/${NODE_TAR}
	tar -xvf ${DOWNLOAD_DIR}/${NODE_TAR} && touch ${NODE_DIR}/.node_extr
fi

export CC=/code/scripts/clang_hextype_castsan
export CXX=/code/scripts/clang++_hextype_castsan

export CFLAGS="-flto -fsanitize=hextype -femit-ivtbl -femit-cast-checks"
export CXXFLAGS="${CFLAGS}"

export LDFLAGS="${CFLAGS} -B/code/binutils/build/gold -Wl,-plugin-opt=save-temps -Wl,-plugin-opt=sd-ivtbl -L/code/libdyncast"
export AR="/code/scripts/ar"


cd ${NODE_DIR}
./configure
make
