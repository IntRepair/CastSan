#include "classes.h"
#include <iostream>

X::X() {
  std::cout << "constructing X...\n";
  x = 1;
}

Y::Y() {
  std::cout << "constructing Y...\n";
}

Z::Z() {
  std::cout << "constructing Z...\n";
}

A::A() {
  std::cout << "constructing A...\n";
  a = "a";
}
B::B() {
  std::cout << "constructing B...\n";
  a = "ba";
  b = "b";
}
C::C() {
  std::cout << "constructing C...\n";
  a = "ca";
  c = "c";
}

D::D() {
  std::cout << "constructing D...\n";
  a = "da";
  b = "d";

}

E::E() {
  std::cout << "constructing E...\n";
}

void Y::p() { std::cout << "Y::p" << std::endl; }
void Z::p() { std::cout << "Z::p" << std::endl; }

void A::f() { std::cout << "A::f" << A::a << std::endl; }

void B::h() { std::cout << "B::h" << B::b << std::endl; }
void B::f() { std::cout << "B::f" << B::a << std::endl; }

void C::e() { std::cout << "C::e" << C::c << std::endl; }

void E::i() { std::cout << "E::i" << std::endl; }
