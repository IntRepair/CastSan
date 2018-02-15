#include "classes.h"
#include <iostream>

/********************* A1 ***********************/
A1::A1() {

	std::cout << "Constructor: A1" << std::endl;

}

A1::~A1() {

	std::cout << "Destructor: A1" << std::endl;

}

void A1::f1() {

	std::cout << "A1::f1" << std::endl;

}

/********************* A2 ***********************/
A2::A2() {

	std::cout << "Constructor: A2" << std::endl;

}

A2::~A2() {

	std::cout << "Destructor: A2" << std::endl;

}

void A2::f2() {

	std::cout << "A2::f2" << std::endl;

}

/********************* A3 ***********************/
A3::A3() {

	std::cout << "Constructor: A3" << std::endl;

}

A3::~A3() {

	std::cout << "Destructor: A3" << std::endl;

}

void A3::f3() {

	std::cout << "A3::f3" << std::endl;

}

/********************* B ************************/
B::B() {

	std::cout << "Constructor: B" << std::endl;

}

B::~B() {

	std::cout << "Destructor: B" << std::endl;

}

void B::b() {

	std::cout << "B::b" << std::endl;

}

void B::f2() {

	std::cout << "B::f2" << std::endl;

}

void B::f3() {

	std::cout << "B::f3" << std::endl;

}

/********************* X ************************/
X::X() {

	std::cout << "Constructor: X" << std::endl;

}

X::~X() {

	std::cout << "Destructor: X" << std::endl;

}

void X::b() {

	std::cout << "X::b" << std::endl;

}

void X::f2() {

	std::cout << "X::f2" << std::endl;

}

void X::f3() {

	std::cout << "X::f3" << std::endl;

}

void X::x() {

	std::cout << "X::x" << std::endl;

}

/********************* C ************************/
C::C() {

	std::cout << "Constructor: C" << std::endl;

}

C::~C() {

	std::cout << "Destructor: C" << std::endl;

}

void C::b() {

	std::cout << "C::b" << std::endl;

}

void C::f1() {

	std::cout << "C::f1" << std::endl;

}

void C::f2() {

	std::cout << "C::f2" << std::endl;

}

void C::f3() {

	std::cout << "C::f3" << std::endl;

}

void C::c() {

	std::cout << "C::c" << std::endl;
}
