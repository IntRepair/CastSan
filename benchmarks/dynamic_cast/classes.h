#ifndef __CLASSES_H__
#define __CLASSES_H__

#include <iostream>

class Base {virtual void member(){}};
class Derived : Base { int kk; };

class parent {
public:
  int t;
  int tt[100];
  virtual int foo() { int a[100]; }
};

class child : public parent, public Derived {
public:
	virtual void member(){ m[0] = 1;};
  int m[1000];
};


#endif
