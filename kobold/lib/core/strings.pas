unit core.strings;
{ Pure string helpers, built entirely over the string builtins (Length, Copy, Pos, Chr, Ord,
  and concatenation) with no runtime primitive of its own, so the unit works on every tier
  and the compiler core carries no knowledge of it.  Reached qualified as strings.* (uses
  core.strings), so the string Delete/Insert do not shadow the dynamic-array builtins of the
  same name.  Case conversion is ASCII only: a byte outside a-z / A-Z is left untouched, which
  keeps the operation UTF-8 safe for the bytes it does not map. }
interface

function UpperCase(const s: string): string;        { ASCII a-z -> A-Z }
function LowerCase(const s: string): string;         { ASCII A-Z -> a-z }
function TrimLeft(const s: string): string;          { drop leading bytes <= space }
function TrimRight(const s: string): string;         { drop trailing bytes <= space }
function Trim(const s: string): string;              { drop both ends }
function StringReplace(const s, oldSub, newSub: string): string;  { every non-overlapping run }
function Delete(const s: string; index, count: integer): string;  { s without count chars at index }
function Insert(const sub, s: string; index: integer): string;    { s with sub starting at index }

implementation

function UpperCase(const s: string): string;
var
  i: integer;
  c: char;
  r: string;
begin
  r := '';
  for i := 1 to Length(s) do begin
    c := s[i];
    if (c >= 'a') and (c <= 'z') then c := Chr(Ord(c) - 32);
    r := r + c;
  end;
  UpperCase := r;
end;

function LowerCase(const s: string): string;
var
  i: integer;
  c: char;
  r: string;
begin
  r := '';
  for i := 1 to Length(s) do begin
    c := s[i];
    if (c >= 'A') and (c <= 'Z') then c := Chr(Ord(c) + 32);
    r := r + c;
  end;
  LowerCase := r;
end;

function TrimLeft(const s: string): string;
var
  i: integer;
begin
  i := 1;
  while (i <= Length(s)) and (s[i] <= ' ') do
    i := i + 1;
  TrimLeft := Copy(s, i, Length(s) - i + 1);
end;

function TrimRight(const s: string): string;
var
  n: integer;
begin
  n := Length(s);
  while (n >= 1) and (s[n] <= ' ') do
    n := n - 1;
  TrimRight := Copy(s, 1, n);
end;

function Trim(const s: string): string;
begin
  Trim := TrimRight(TrimLeft(s));
end;

function StringReplace(const s, oldSub, newSub: string): string;
var
  r, rest: string;
  p, flen: integer;
begin
  { an empty search string matches nothing, so the input is returned unchanged rather than
    looping forever }
  if Length(oldSub) = 0 then begin
    StringReplace := s;
    Exit;
  end;
  r := '';
  rest := s;
  flen := Length(oldSub);
  p := Pos(oldSub, rest);
  while p > 0 do begin
    r := r + Copy(rest, 1, p - 1) + newSub;
    rest := Copy(rest, p + flen, Length(rest) - (p + flen) + 1);
    p := Pos(oldSub, rest);
  end;
  StringReplace := r + rest;
end;

function Delete(const s: string; index, count: integer): string;
var
  n: integer;
begin
  n := Length(s);
  if (index < 1) or (index > n) or (count <= 0) then begin
    Delete := s;
    Exit;
  end;
  { Copy clamps a count that runs past the end, so the tail slice is safe }
  Delete := Copy(s, 1, index - 1) + Copy(s, index + count, n - index - count + 1);
end;

function Insert(const sub, s: string; index: integer): string;
var
  n: integer;
begin
  n := Length(s);
  if index < 1 then index := 1;
  if index > n + 1 then index := n + 1;
  Insert := Copy(s, 1, index - 1) + sub + Copy(s, index, n - index + 1);
end;

end.
