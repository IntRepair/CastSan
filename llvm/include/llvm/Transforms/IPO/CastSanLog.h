#ifndef LLVM_TRANSFORMS_IPO_CASTSAN_LOG_H
#define LLVM_TRANSFORMS_IPO_CASTSAN_LOG_H

#include <iostream>
#include <cstdio>
#include "llvm/Support/raw_ostream.h"
#include <execinfo.h>
#include <stdarg.h>

#define SD_DEBUG

//Paul: this is the default terminal printing method
static void sd_print(const char* fmt, ...) {
#ifdef SD_DEBUG
  va_list args;
  va_start(args,fmt);
  fprintf(stderr, "SD] ");
  vfprintf(stderr, fmt,args);
  va_end(args);
  fflush(stderr);
#endif
}

#endif

