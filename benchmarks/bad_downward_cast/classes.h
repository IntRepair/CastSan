#ifndef __CLASSES_H__
#define __CLASSES_H__

#include <iostream>

struct A {
  A();
  virtual void f ();
  std::string a;
};

struct B : A {
  B();
  void h ();
  void f ();
  std::string b;
};

#endif
