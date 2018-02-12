#include "classes.h"
#include <iostream>
#include <stdlib.h>
#include <time.h>

A * randomBorC() {

  int rand_int = rand();
  int n = rand_int % 2;
  if(n == 0)
    return new D();
  else
    return new C();
}

int main(int argc, char *argv[])
{

  D* asd = new D();
  E* e = static_cast<E*>(asd);

  A* a_one = new C();
  B* b_one = static_cast<B*>(a_one);

  e->i();

  srand(time(NULL));
  A* a;
  D* d;
  std::string test = "";
  bool atLeastOneC = false;
  for (int i = 0; i < 5 || !atLeastOneC; i++) {
    a = randomBorC();

    d = static_cast<D*>(a);
    std::cout << "casted\n";
    d->f();
    std::cout << "called\n";
    if(d->b == "c")
      atLeastOneC = true;
  }

  return 0;
}
