#include "classes.h"
#include <iostream>

A::A() {std::cout << "constructing A...\n";}
B::B() {std::cout << "constructing B...\n";}

void A::f() { std::cout << "A::f" << A::a << std::endl; }

void B::h() { std::cout << "B::h" << B::b << std::endl; }
void B::f() { std::cout << "B::f" << B::a << std::endl; }

Z::Z() {std::cout << "constructing Z...\n";}
Y::Y() {std::cout << "constructing Y...\n";}

void Z::f() { std::cout << "Z::f" << std::endl; }
void Y::f() { std::cout << "Y::f" << std::endl; }

void Y::h() { std::cout << "Y::h" << std::endl; }
