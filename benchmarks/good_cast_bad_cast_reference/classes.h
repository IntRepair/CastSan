#ifndef __CLASSES_H__
#define __CLASSES_H__

#include <iostream>

struct A {
  virtual void f ();
};

struct B : A {
  void f ();
};


#endif
