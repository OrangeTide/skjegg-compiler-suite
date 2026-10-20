unit io.dirs;
{ Directory reads as a unit a program `uses`, the companion to `files`.  The
  compiler core carries no directory knowledge: the primitives are ordinary
  external symbols the runtime provides (kpd_open / kpd_next / kpd_close, in
  kobold/aot_rt.c, kobold/io_free.c and the JIT binding table), declared once
  here and wrapped in a typed Kobold interface.

  Open a directory, then call NextEntry until it returns False.  Each call
  yields one entry name; the "." and ".." entries are skipped, so a caller sees
  only real names.  Entry order is whatever the filesystem returns, not sorted.
  Line buffering of the raw directory block is done in the runtime, so the host
  seam stays one name at a time on every tier. }
interface

type Directory = record
  handle: integer;    { table index of the open directory, -1 when closed }
end;

function OpenDir(const path: string; var d: Directory): boolean;
function NextEntry(var d: Directory; var name: string): boolean;
procedure CloseDir(var d: Directory);

implementation

{ The host primitives, declared once here instead of in every program. }
function kpd_open(const path: string): integer; external cdecl;
function kpd_next(handle: integer): string; external cdecl;
procedure kpd_close(handle: integer); external cdecl;

{ True for the "." and ".." pseudo-entries, which NextEntry hides. }
function isdot(const s: string): boolean;
begin
  isdot := ((length(s) = 1) and (s[1] = '.'))
        or ((length(s) = 2) and (s[1] = '.') and (s[2] = '.'));
end;

{ Open a directory for reading.  True on success. }
function OpenDir(const path: string; var d: Directory): boolean;
begin
  d.handle := kpd_open(path);
  OpenDir := d.handle >= 0;
end;

{ Read the next entry name into `name`, skipping "." and "..".  Returns False
  when the directory is exhausted (and `name` is then empty). }
function NextEntry(var d: Directory; var name: string): boolean;
begin
  name := kpd_next(d.handle);
  while isdot(name) do
    name := kpd_next(d.handle);
  NextEntry := length(name) > 0;
end;

{ Close the handle; safe to call more than once. }
procedure CloseDir(var d: Directory);
begin
  if d.handle >= 0 then
    kpd_close(d.handle);
  d.handle := -1;
end;

end.
