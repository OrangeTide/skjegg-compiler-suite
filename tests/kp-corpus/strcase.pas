program strcase;
{ A case statement on a string selector matches each label by value. This covers a
  borrowed selector (a variable) with multiple labels per arm, an empty-string label, and
  an else default, then an owned selector (a fresh concatenation, matched then released).
  Run under kprun it prints north north quit look empty other N n ., one per line. }
var
  words: array[0..5] of string;
  i: integer;
begin
  words[0] := 'north';
  words[1] := 'n';
  words[2] := 'quit';
  words[3] := 'look';
  words[4] := '';
  words[5] := 'zork';
  for i := 0 to 5 do
    case words[i] of
      'north', 'n': writeln('north');
      'quit': writeln('quit');
      'look': writeln('look');
      '': writeln('empty');
    else
      writeln('other');
    end;
  for i := 0 to 2 do
    case words[i] + '!' of
      'north!': writeln('N');
      'n!': writeln('n');
    else
      writeln('.');
    end;
end.
