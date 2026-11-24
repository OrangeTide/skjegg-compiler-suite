{ Typed pointers with new/dispose.  A pointer is a machine-word address; ^T
  declares a pointer type, p^ dereferences, new(p) allocates sizeof(p^) and
  dispose(p) releases it.  nil is the null pointer.  This exercises a scalar
  pointer and a singly linked list of records built with new and walked to
  nil, including p^.field and p^.next chaining. }
program PointerDemo;
type
  PInt = ^integer;
  PNode = ^Node;
  Node = record
    val: integer;
    next: PNode;
  end;
var
  p: PInt;
  head, cur, nxt: PNode;
  i: integer;
begin
  new(p);
  p^ := 42;
  writeln(p^);                   { 42 }
  p^ := p^ + 1;
  writeln(p^);                   { 43 }
  dispose(p);

  { build the list 1 -> 2 -> 3 by prepending 3, 2, 1 }
  head := nil;
  for i := 3 downto 1 do
  begin
    new(cur);
    cur^.val := i;
    cur^.next := head;
    head := cur
  end;

  { walk it, disposing each node once printed so no storage leaks }
  cur := head;
  while cur <> nil do
  begin
    writeln(cur^.val);           { 1, 2, 3 }
    nxt := cur^.next;
    dispose(cur);
    cur := nxt
  end
end.
