#include "classes.h"
#include <iostream>

int main(int argc, char *argv[])
{
  B* b = new B();
  
  C* c = static_cast<C*>(static_cast<A*>( b));

  std::cerr << "bad casting works: " << c->a << "\n";

  return 0;
}
