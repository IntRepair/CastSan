#include "classes.h"
#include <iostream>

int main(int argc, char *argv[])
{
  B* b = new B();
  b->b = "b";

  A* a = reinterpret_cast<A*>(b);

  a->f();

  return 0;
}
