program propertyhandle;
{ Handle-typed properties (doc/languages/kobold/kp-property.md): the accessor result is a
  reference-counted handle, not a plain scalar. A read is a getter call that transfers an
  owned reference (so it is released like any call result), a write a setter call. Covers a
  read/write string property, a read-only computed string property, and a computed string
  record field, including a getter read used inside a concatenation and a setter fed an
  owned temporary. Run under kprun it prints, one per line:
    hello
    world
    world!
    hi
    Ada Lovelace }

var
  backingName: string;

function getName: string;
begin
  getName := backingName;
end;

procedure setName(const s: string);
begin
  backingName := s;
end;

function getGreeting: string;
begin
  getGreeting := 'hi';
end;

{ a read/write string property and a read-only computed one }
property name: string read getName write setName;
property greeting: string read getGreeting;

type
  Person = record
    first, last: string;
    full: string read getFull;   { computed: "first last" }
  end;

function getFull for (p: Person): string;
begin
  getFull := p.first + ' ' + p.last;
end;

var
  p: Person;
  s: string;

begin
  name := 'hello';
  writeln(name);              { getter read }
  name := 'wor' + 'ld';       { setter fed an owned concatenation }
  s := name;                  { getter transfers ownership; s owns its copy }
  writeln(s);
  writeln(name + '!');        { getter read inside a concatenation }
  writeln(greeting);          { read-only computed property }

  p.first := 'Ada';
  p.last := 'Lovelace';
  writeln(p.full);            { computed record field through its getter method }
end.
