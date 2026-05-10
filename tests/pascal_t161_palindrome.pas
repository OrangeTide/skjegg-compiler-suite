{ Test palindrome check }
program TestPalindrome;
var
  s: string[20];

function isPalindrome(var str: string[20]): boolean;
var
  i, n: integer;
begin
  n := length(str);
  isPalindrome := true;
  for i := 1 to n div 2 do
    if str[i] <> str[n - i + 1] then begin
      isPalindrome := false;
      exit;
    end;
end;

begin
  s := 'racecar';
  if isPalindrome(s) then writeln('yes') else writeln('no');

  s := 'hello';
  if isPalindrome(s) then writeln('yes') else writeln('no');

  s := 'abba';
  if isPalindrome(s) then writeln('yes') else writeln('no');

  s := 'a';
  if isPalindrome(s) then writeln('yes') else writeln('no');

  s := 'ab';
  if isPalindrome(s) then writeln('yes') else writeln('no');
end.
