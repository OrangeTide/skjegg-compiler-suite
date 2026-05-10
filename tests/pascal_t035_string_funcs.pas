program t035_string_funcs;
var
    s, t, u: string;
    i: integer;
begin
    { copy }
    s := 'hello world';
    t := copy(s, 7, 5);
    writeln(t);

    { copy with out-of-range: past end }
    t := copy(s, 7, 100);
    writeln(t);

    { copy with index past length }
    t := copy(s, 50, 3);
    writeln(length(t));

    { pos }
    i := pos('world', s);
    writeln(i);

    i := pos('xyz', s);
    writeln(i);

    i := pos('hello', s);
    writeln(i);

    { delete }
    s := 'abcdefgh';
    delete(s, 3, 2);
    writeln(s);

    { delete past end }
    s := 'abcdefgh';
    delete(s, 6, 100);
    writeln(s);

    { insert }
    s := 'abcdef';
    t := 'XY';
    insert(t, s, 3);
    writeln(s);

    { insert at beginning }
    s := 'world';
    insert('hello ', s, 1);
    writeln(s);

    { concat function }
    s := 'one';
    t := ' two';
    u := concat(s, t, ' three');
    writeln(u);
end.
