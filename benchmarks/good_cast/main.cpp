#include "classes.h"
#include <iostream>
#include <stdlib.h>
#include <time.h>

template <typename T>
T castToTRef(A & a) {
  return static_cast<T&>(a);
}

template <typename T>
T * castToTPointer(A * a) {
  return static_cast<T*>(a);
}

A * getAOrB() {
  srand(time(NULL));
  int n = rand() % 2;
  if(n == 0)
    return new A();
  else
    return new B();
}

int main(int argc, char *argv[])
{
  /*B b_ref = castToTRef<B>(*new B());
  b_ref.h();

  A a_ref = static_cast<A&>(*new B());
  a_ref = static_cast<B&>(a_ref);
  b_ref.h();

//  B * b2 = castToTPointer<B>(new B());
  A * a = castToTPointer<B>(new B());
  a->f();
  //b2->h();*/

  A * a2 = static_cast<B*>(getAOrB());
  a2->f();

  return 0;
}
