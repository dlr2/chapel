pragma "use default init"
class MyClass { var x:int; }

pragma "use default init"
record Collection {
  type elementType;
  var element: elementType;
}

pragma "unsafe" // TODO
proc Collection.addElement(arg: elementType) {
  element = arg;
}

proc test() {
  var c: Collection(owned MyClass);
  c.addElement( new owned MyClass() ); // transferred to element

  var global = new owned MyClass();

  var d: Collection(int);     d.addElement( 1 ); // OK
  var e: Collection(MyClass); e.addElement(global.borrow()); // OK
}

test();
