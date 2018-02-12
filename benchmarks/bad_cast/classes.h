#ifndef __CLASSES_H__
#define __CLASSES_H__

#include <iostream>

struct A {
  A();
  virtual void f ();
  virtual void g ();
  virtual void h ();
};

struct B: public A {
  int a;
  B();
  void f ();
  void h ();
};

struct C: public A {
  int a;
  C();
  void g ();
  void h ();
};

#endif
