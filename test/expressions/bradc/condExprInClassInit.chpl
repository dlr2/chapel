class C {
  proc postinit() {
    const blo = if (here.id == 0) then 333 else 444;
    writeln("blo is: ", blo);
  }
}

var myC = new unmanaged C();
//myC.foo();
delete myC;
