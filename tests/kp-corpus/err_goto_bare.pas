{ a bare `goto name` is rejected; only a computed jump through a label value
  (`goto @name` into a dispatch table) is allowed, so there is no unstructured
  goto.  The label here is defined before the goto so the checker sees it and
  teaches the computed form. }
program err_goto_bare;
procedure run;
begin
again:
  goto again;
end;
begin
  run
end.
