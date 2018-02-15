
class A1 {
	public:
	A1();

	virtual ~A1();

	virtual void f1();

};


class A2 {
	public:
	A2();

	virtual ~A2();

	virtual void f2();

};


class A3 {
	public:
	A3();

	virtual ~A3();

	virtual void f3();

};

class B : public A2, public A3 {
	public:
	B();

	virtual ~B();

	virtual void f2();
	virtual void f3();

	virtual void b();

};


class C : public A1, public B {
	public:
	C();

	virtual ~C();

	virtual void f1();
	virtual void f2();
	virtual void f3();

	virtual void b();

	virtual void c();

};

class X : public B {
	public:
	X();

	virtual ~X();

	virtual void f2();
	virtual void f3();
	virtual void b();

	virtual void x();
};
