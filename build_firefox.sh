#!/bin/bash

cd /home/llvm/source/firefox-53.0.3

rm -f mozconfig
touch mozconfig

CFLAGS="-flto -fuse-ld=gold -fsanitize=hextype -femit-ivtbl -femit-cast-checks"
(
	echo "export CC=/code/build/bin/clang"
	echo "export CXX=/code/build/bin/clang++"
	
	echo "export CFLAGS=\"${CFLAGS} -O1\""
	echo "export CXXFLAGS=\"${CFLAGS} -O1\""
	echo "export LDFLAGS=\"${CFLAGS} -O1\""
	echo "export LD_LIBRARY_PATH=/code/build/lib:/usr/lib/x86_64-linux-gnu"

	echo "export AR=/code/scripts/ar"
) > mozconfig

cat /code/mozconfig >> mozconfig

echo "" >> mozconfig
echo "mk_add_options MOZ_OBJDIR=/code/setup_dir/firefox" >> mozconfig

export MOZCONFIG=${PWD}/mozconfig
export SHELL=/bin/bash
export PATH=/code/build/bin/:$PATH

#cp /code/old-configure ./old-configure
#cp /code/moz.build mozglue/misc/moz.build
#cp /code/Makefile.in ./Makefile.in
#cp /code/pixman_compiler.h ./gfx/cairo/libpixman/src/pixman-compiler.h

#rm /code/setup_dir/firefox/config.status
#./old-configure
make -k -f client.mk
