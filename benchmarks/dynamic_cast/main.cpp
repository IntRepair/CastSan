#include "classes.h"

void normal_case() {
  Base test2;
  Base *derivedPtr = &test2;
  child *result = dynamic_cast<child *>(derivedPtr);
  result->member();
  // CHECK: call i8* @__dynamic_casting_verification
}

int main() {
  normal_case();
  return 1;
}
