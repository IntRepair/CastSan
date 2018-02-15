#include "classes.h"
#include <iostream>

int main(int argc, char *argv[])
{
  A* a = new A();
  B* b = new B();
  a->a = "a";
  b->a = "b";

  B* bad_b = static_cast<B*>(a);
  b->h();
  b->f();
  bad_b->f();

  return 0;
}
