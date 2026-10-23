{ Write/WriteLn stream selection and Flush (P6 stage B).  A leading Output or
  ErrOutput argument selects the stream (stdout fd 1 / stderr fd 2); output is
  unbuffered so Flush is a no-op.  The harness checks stdout, so the ErrOutput
  lines go to stderr and only the stdout lines are asserted here. }
program Streams;
begin
  writeln('out1');
  writeln(ErrOutput, 'err1 ', 99);
  write(ErrOutput, 'err2');
  writeln(ErrOutput);
  Flush(ErrOutput);
  writeln(Output, 'out2');
end.
