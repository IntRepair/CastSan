#include "classes.h"
#include <iostream>

int main (int argc, char ** argv) {

	X * x = new X();
	A3 * a3 = static_cast<A3*>(x);

	a3->f3();

}
