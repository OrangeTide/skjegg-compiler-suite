program t090_define_undef;

{ CPAS is predefined by the compiler }
{$IFDEF CPAS}
const Greeting = 'cpas';
{$ELSE}
const Greeting = 'other';
{$ENDIF}

{ DEFINE adds a symbol; case-insensitive }
{$DEFINE FOO}
{$IFDEF foo}
const HasFoo = 'y';
{$ELSE}
const HasFoo = 'n';
{$ENDIF}

{ UNDEF removes a symbol }
{$UNDEF FOO}
{$IFNDEF FOO}
const NoFoo = 'y';
{$ELSE}
const NoFoo = 'n';
{$ENDIF}

begin
  writeln(Greeting);
  writeln(HasFoo);
  writeln(NoFoo);
end.
