program t030_string_compare;
var
    a, b: string;
begin
    a := 'apple';
    b := 'banana';

    { equality }
    if a = a then writeln(1) else writeln(0);
    if a = b then writeln(1) else writeln(0);
    if a <> b then writeln(1) else writeln(0);

    { ordering }
    if a < b then writeln(1) else writeln(0);
    if b < a then writeln(1) else writeln(0);
    if a > b then writeln(1) else writeln(0);
    if b >= a then writeln(1) else writeln(0);
    if a <= a then writeln(1) else writeln(0);

    { compare with literal }
    if a = 'apple' then writeln(1) else writeln(0);
    if a < 'banana' then writeln(1) else writeln(0);

    { length decides when prefix matches }
    a := 'abc';
    b := 'abcd';
    if a < b then writeln(1) else writeln(0);
    if b > a then writeln(1) else writeln(0);

    { empty string }
    a := '';
    if a = '' then writeln(1) else writeln(0);
    if a < b then writeln(1) else writeln(0);
end.
