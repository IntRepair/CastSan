#ifndef __CLASSES_H__
#define __CLASSES_H__

#include <iostream>

struct A {
  int s;
  A();
  virtual void f ();
  std::string a;
  int z;
};

struct B : A {
  B();
  void h ();
  virtual void f ();
  std::string b;
};

struct Z {
  Z();
  void f ();
  std::string a;
};

struct Y : Z {
  Y();
  void f ();
  void h ();
  std::string b;
};

#endif
