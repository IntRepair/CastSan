#include "classes.h"
#include <iostream>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[])
{
  A * b_as_a = new B();

  B * real_b = static_cast<B*>(b_as_a);

  std::cout << "legal casting works... :)" << std::endl;

  real_b->f();

  A * a = new A();
  B * b = static_cast<B*>(a);
  B * b2 = dynamic_cast<B*>(a);

  std::cout << "illegal casting works... " << std::endl;

  b->f();

  return 0;
}
