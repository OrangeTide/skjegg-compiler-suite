# Runtime layering: what C and Kobold share, and where they split

The sandbox runtime is language-agnostic. A C program and a Kobold program that
target the same machine run on one shared bottom layer and reach the host
through one file system seam. This note records what the two runtimes share on
purpose, and the two places they stay separate on purpose, so a later change
keeps the boundary where it belongs rather than merging code that genuinely
differs or duplicating code that should be one source.

## The shared bottom layer

These pieces are one implementation, linked by both the C libc and the Kobold
runtime across the tiers that apply:

- **Process entry and the argv contract.** One `_start` reads `argc` and the
  argv vector off the entry stack and calls `main(argc, argv)`. A C program uses
  it directly; a Kobold guest's `main` records the vector for `ParamStr` /
  `ParamCount` and runs the program. The crt is per target (the syscall seam is
  the one target-specific piece), the contract is shared.
- **Number formatting (`libc/numfmt.c`).** `__fmt_udec` and `__fmt_greal` are
  the libgcc-free integer and `%g` renderers. The C libc `printf` and the Kobold
  console writer share them, so an integer or a real prints the one way whatever
  language emitted it, and the 32-bit tiers need no 64-bit divide helper.
- **The heap bump (`sbrk`).** One brk-bump allocator underlies both the C
  `malloc` arena and the Kobold guest heap.
- **Randomness (`rng`).** One generator backs both runtimes' random surface.
- **The file system switch (`fs/`).** The mount table, the fd space, the path
  resolver, and the backends are plain C that neither language owns. A C libc
  path call and a Kobold file/directory primitive are both thin adapters over
  the same `fs_open`/`fs_read`/`fs_write`/`fs_close` and the same
  `fs_dirent` directory model. See `fs/fs.h` and `doc/tools/process-model.md`.

## The one file and directory model

Every tier now reaches files and directories through `fs/`. There is no
`opendir`/`DIR*` layer and no raw `open` in the language runtimes; a directory
is an fd opened with `FS_O_DIRECTORY` and read as a whole number of fixed
`struct fs_dirent` records, and a language's `OpenDir`/`NextEntry` is a thin
wrapper over `fs_open`/`fs_read`/`fs_close`.

What differs per tier is only the backend behind the mount and how the mount
table is populated, not the model or the call surface:

- **Freestanding guest** (`kobold/io_free.c`, the C libc under
  `SKJ_FS_INTERPOSE`): the launcher fills the mount table with capability
  grants, or, under bare qemu, `fs_init` installs a scratch ramfs and the
  optional ambient host mount. Host access is capability-confined
  (`hostcap`), with the `ambient` `AT_FDCWD` backend as a documented stop-gap.
- **Hosted Kobold** (`kobold/kp_rt_io.c`, the in-process JIT and the AOT
  archives): links `fs/fs.c` + `fs/hostfs.c` and, on the first file or directory
  call, mounts a `hostfs` root at `/` as the current mount and sets its current
  directory to the process cwd. So an absolute path resolves from `/` and a
  cwd-relative path from the working directory, exactly as the old native
  `open` did, while the fd and directory model is the shared one. A `..`
  component is refused, as in the sandbox.

A console fd (0, 1, 2) is never fs-managed. A unit that opens one directly and
hands it to the byte primitives goes to the raw read/write seam; `fs` fds start
at `FS_FD_BASE` (3), so the two never collide.

## The two deliberate seams

These stay separate because the tiers genuinely differ, not for lack of
unification:

- **Whole-file read/write allocation model.** `ReadFileStr`/`WriteFileStr`
  stay on the per-tier whole-file helpers (`kp_read_file`/`kp_write_file`).
  The freestanding tier allocates the read buffer from the guest heap and builds
  the result string in place; the hosted tiers use host `malloc`. That
  allocation split is the reason these two entry points are not folded onto the
  `fs/` fd primitives, which carry no buffer.
- **Stdio shape: C `FILE*` versus Kobold fd.** The C libc keeps buffered
  `FILE*` stream objects with their own line discipline. The Kobold console and
  `files` surface is unbuffered and fd-based (`kp_io.c` writes straight to the
  fd, with one byte of read pushback owned by the runtime). The two sit on the
  same `fs/` and raw-seam calls underneath, but the stream object and its
  buffering are not shared.

The goal is one documented boundary: shared where the tiers do the same thing,
split only where they do not.
