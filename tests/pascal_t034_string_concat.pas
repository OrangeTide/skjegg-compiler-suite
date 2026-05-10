program t034_string_concat;
var
    a, b, c: string;
    t: string[10];
begin
    { basic concat }
    a := 'hello';
    b := ' world';
    c := a + b;
    writeln(c);
    writeln(length(c));

    { concat with literal }
    c := a + '!';
    writeln(c);

    { chained concat }
    c := 'one' + ' ' + 'two';
    writeln(c);

    { concat into short string (truncation) }
    t := 'abcde' + 'fghijklmnop';
    writeln(t);
    writeln(length(t));

    { concat with empty }
    c := a + '';
    writeln(c);
    c := '' + b;
    writeln(c);
end.
