#ifndef __CLASSES_H__
#define __CLASSES_H__

#include <iostream>

struct Y {
  Y();
  virtual void p();
};

struct X {
  X();
  int x;
};

struct Z : X,Y {
  Z();
  void p();
};

struct A : Z {
  A();
  virtual void f ();
  std::string a;
};

struct B : A {
  B();
  virtual void h ();
  void f ();
  std::string b;
};

struct C : A {
  C();
  virtual void e ();
  std::string c;
};

struct E {
  E();
  virtual void i ();
};

struct D : B, E {
  D();
};


#endif
