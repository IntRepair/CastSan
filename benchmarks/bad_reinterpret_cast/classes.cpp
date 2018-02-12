#include "classes.h"
#include <iostream>

A::A() {std::cout << "constructing A...\n";}
B::B() {std::cout << "constructing B...\n";}

void A::f() { std::cout << "A::f" << A::a << std::endl; }

void B::h() { std::cout << "B::h" << B::b << std::endl; }
