@ok
<?php
require_once "polyfills.php";

class Foo {
  public $foo = "foo";
}

class Bar extends Foo {
  public $bar = "bar";
}

class Baz extends Bar {
  public $baz = "baz";
}

interface IX {}

class X implements IX {
  public $x = "x";
}

class Y extends X {
  public $y = "y";
}

class Z extends Y {
  /** @var (tuple(Foo,X)|false)[] */
  public $at = [];
}

function simple_case() {
  /** @var Foo */
  $foo = new Foo();
  $foo = new Baz();

  var_dump(instance_to_array($foo));
}

function complex_case() {
  $g = new Z;
  $g->at[] = tuple(new Baz, new X);
  $g->at[] = false;
  $g->at[] = tuple(new Foo, new Y);
  $g->at[] = tuple(new Bar, new X);

  var_dump(instance_to_array($g));
}

simple_case();
complex_case();
