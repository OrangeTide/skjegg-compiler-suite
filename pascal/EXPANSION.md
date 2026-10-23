# skj-pc expansion plan

Goal: grow skj-pc from the Compact Pascal subset into a general purpose Pascal
compiler covering a subset of ISO Extended Pascal and FPC Object Pascal large
enough to correctly compile complex real programs. The acceptance program is
kobold/kpcom.pas (the Kobold compiler, ~12.8k lines, built with KOBOLD defined).

## Decisions

- Native AOT only, host targets. Backends: x86-64 and aarch64 are required and
  mirror the skj-cc multi-backend setup (one frontend, one binary per target
  linked with that target's selector). ColdFire output stays working. Other
  backends are stretch, reusing the skj-cc machinery.
- Use the host libc. The "runtime" is one C glue file over malloc/stdio, the
  role runtime/pascal_rt.c already fills on the ColdFire path.
- Managed types (string, dynamic array) are reference counted, FPC ansistring
  style: the compiler emits retain/release at assignment and scope exit.
- kpcom.pas is compiled with KOBOLD defined, so its I/O is `external cdecl`
  host imports, not SysUtils and not a `file` type.

## Target model

- LP64 for x86-64 and aarch64: pointers and managed descriptors are 8 bytes,
  Int64 is 8 bytes, Integer/LongInt stay 32 bits. The frontend selects pointer
  width from a compile-time flag, as the C front end does with CC_LP64. The
  ColdFire path stays ILP32 with 4-byte slots.
- Program entry is renamed off `main` on the host build; a C crt owns
  `main(argc, argv)`, captures the args for ParamStr/ParamCount, and calls the
  Pascal entry.

## Phases

- [x] P0 Host AOT pipeline on the current subset. build/skj-pc-x86-64 (NASM)
      and build/skj-pc-arm64 (GNU-as via cross cc). Host runtime
      (runtime/pascal_rt.c built with -DPAS_HOST, libc-backed). check-pc-host
      and check-pc-arm64 run the existing tests/pascal_*.pas natively via
      tests/run-pc-host.sh. DONE: 190/190 on both host backends.
- [x] P1 Type layer + scalar widening.
      DONE: LP64 pointer-width layer. The front end emits the 64-bit address
      opcodes (IR_LEA64, IR_ADL64, IR_LDL64, IR_STL64, IR_ADD64, IR_ARG64) under
      PC_LP64, keeps pointer-carrying slots (var/const-aggregate/value-aggregate
      params, captures, with-base, set/string temps) 8 bytes, sign-extends a
      32-bit address offset before a 64-bit add, and passes pointer arguments as
      IR_ARG64 via a per-temp pointer-flag table.  Fixed a latent bug (a
      compound value parameter's incoming slot must be pointer-sized), harmless
      on ILP32 but a crash on LP64.
      DONE: type descriptor + Int64.  The scalar unit is 4 bytes; Int64 and
      QWord are 8-byte scalars.  type_bytes()/is_i64_type_name() resolve a type
      name (built-in, alias chain, subrange) to a size, and record fields, array
      elements, variable/parameter slots and globals size from it.  A per-temp
      i64 flag tracks 64-bit values; binary ops promote to the IR 64-bit opcodes
      (arith/bitwise/shift/compare/neg), loads and stores pick LD64/ST64 and
      LDL64/STL64, a literal beyond 32 bits becomes IR_LIC64, Int64()/QWord()
      sign-extend and Integer()/LongInt() truncate, and an Int64 result returns
      through IR_RETV64 / is called through IR_CALL64 (the backends read result
      width from the opcode).  type aliases (`type X = Y;`) and subranges
      (`type X = lo..hi;`) parse and register.  Runtime: __pascal_write_int64,
      __pascal_str_from_int64.  tests/pascal_t224_int64.pas covers records,
      arrays, params, results, casts, shifts and bitwise-not.
      x86-64 191/191, aarch64 191/191, ColdFire 262/262 (register-pair i64).
      DONE: Real/Double (f64) and Single (f32).  The lexer reads real literals
      (T_REAL), the parser an N_REAL node, and `/` is parsed and always yields a
      real (Pascal semantics).  float_fw_of_name() classifies real/double/single
      (and aliases); type_bytes sizes them (8/8/4).  A per-temp float-kind table
      drives the float opcodes: FADD/FSUB/FMUL/FDIV, FCMP*, FNEG, FLD/FSD and
      FLDL/FSTL loads+stores (imm = width), ITOF/FTOI and F32TOF64/F64TOF32
      conversions, FARG arguments, FRETV returns and FCALL results.  Float
      scalar parameters set fn->param_cls/param_neb/param_fw/param_class (class
      1 = SSE) threaded to the register ABI, mirroring the C front end; float
      globals use base_type IR_F32/IR_F64.  Builtins: trunc, round, int, sqrt,
      and real()/single() casts.  Runtime (host, libc-backed): __pascal_write_
      real, __pascal_str_from_real, __pascal_round, __pascal_sqrt; the
      freestanding runtime keeps link-complete stubs since the ColdFire corpus
      has no reals, so the _real tests are host-only (excluded from the m68k
      binary builds and check-as).  tests/pascal_t225_real.pas covers records,
      arrays, params, results, mixed int/real, casts, trunc/round/sqrt and
      comparisons.  x86-64 192/192, aarch64 192/192, ColdFire 262/262.
- [x] P2 Heap strings. Replace the 255-byte shortstring with a refcounted heap
      string. Rewrite the __pascal_str_* helpers for heap + refcount. Emit
      retain/release for managed locals, parameters, fields, and results.
      Self-append (x := x + e) in place. String-typed function results.
      DONE across stages 0-5 below; the memory model is reference counted,
      copy-on-write and leak-clean under the live ASan + LeakSanitizer gate.
      Staged so each commit keeps the tree green, separating the value
      representation from the memory management:
      - [x] Stage 0: a leak gate.  check-pc-host-leak runs the x86-64 host
            suite under AddressSanitizer + LeakSanitizer (run-pc-host.sh SAN=1,
            RT = build/pascal_rt_host_asan.o), so a test also fails on a leak,
            double free or use after free.  Native x86-64 only (LSan is
            unreliable under qemu-user).  Baseline 192/192 (no heap use yet).
      - [x] Stage 1: the string value is now a pointer to a heap descriptor,
            the Kobold {refcount, kind, len, cap} object (16-byte header, bytes
            at +16, NULL = '').  A literal is a static strobj global (immortal
            refcount), endian-correct on every backend.  The __pascal_str_*
            helpers allocate and return heap strings (concat/copy/delete/insert
            are value-returning, delete/insert/str/read rebind the destination);
            load/store treat a string as a pointer scalar, s[i] indexes the
            descriptor data, string value params and assignment deep-copy
            (__pascal_str_dup), and string function results return the pointer.
            The freestanding runtime gets a static bump allocator (host uses
            malloc).  No reference counting yet, so everything leaks by design.
            string[N] now behaves as an unbounded ansistring (the bound is
            ignored): ShortString truncation, the s[0] length byte and writing
            past the current length are gone; the affected tests were updated
            (t029/t031/t033/t034/t128 non-truncating, t181/t206/t213 rebuilt on
            the s := s + x idiom and copy(), t089 retargeted to a 4-vs-8
            alignment check).  Gates: x86-64 194/194, aarch64 194/194, ColdFire
            262/262, full m68k check 262/262, compiler ASan 225/225.  The leak
            gate reports only LeakSanitizer leaks (no overflow/UAF/invalid-free)
            and is still expected-red, not yet wired into check.
      - [x] Stage 2: reference counting.  __pascal_str_retain and
            __pascal_str_release maintain the descriptor refcount (a NULL and a
            static literal are immortal; release frees at zero on the host, the
            freestanding bump arena reclaims only at exit).  Assignment now
            shares by reference count instead of deep copying: it releases the
            destination's previous value and either retains a borrowed source
            or moves an owned one.  Ownership is classified syntactically
            (str_rvalue_owned): a literal and a concatenation are owned
            (moved), everything else is borrowed (retained), the sound
            direction since over-retaining only leaks while under-retaining
            would free too early.  Retain is emitted before the old value's
            release so x := x stays alive.  String value parameters are
            retained on entry (the callee owns its reference).  In-place
            character mutation (s[i] := c) now needs copy on write, brought
            forward from stage 5: __pascal_str_unique returns the value when it
            is the sole owner, else a private copy with the shared reference
            dropped, moved back into the slot before the byte is addressed.
            Gates (value): x86-64 194/194, aarch64 194/194, ColdFire 262/262,
            full m68k 262/262, compiler ASan 225/225.  The leak gate reports
            only LeakSanitizer leaks (no overflow/UAF/double-free/invalid-free),
            now down to 6 tests, and stays expected-red until stage 3.
      - [x] Stage 3: scope-exit release and the leak gate turned on.  Every
            function releases its managed string locals and its retained
            string value parameters on the way out (after exit_label, where
            fall-through and Exit both land); the result slot is excluded so a
            string result's reference transfers to the caller.  Expression
            temporaries (an intermediate concatenation, a string compared in a
            condition, an argument built on the fly) are tracked in a
            statement-scoped registry: each owned string allocation is
            registered, a store that moves it adopts it (unregisters), and
            every statement brackets its lowering with a mark and a release of
            whatever is still registered.  A while/until condition releases its
            temporaries before the branch, so both the continue and exit paths
            free them and nothing accumulates across iterations.  Globals stay
            reachable through their slot and so are not reported; a reassigned
            heap value is released on overwrite.  check-pc-host-leak is now a
            prerequisite of check-pc-host (the x86-64 host suite always runs
            under ASan + LSan).  Gates: host leak 194/194 and value 194/194,
            aarch64 194/194, ColdFire 262/262, full m68k 262/262, compiler
            ASan 225/225.  (Halt leaves through libc exit without running the
            releases; it ends the process, so it is not a real leak, though a
            hypothetical Halt-while-holding-strings test would need handling.)
      - [x] Stage 4: managed members of aggregates.  A compile-time walk
            (emit_managed_walk over type_has_managed types) retains, releases
            or null-inits every string inside a record field or array element,
            recursively.  A record or array assignment (emit_aggregate_copy)
            retains the source's managed members and releases the
            destination's old ones before the block copy, so sharing a record
            with a string field is reference-counted and reassigning one copy
            no longer dangles the other (the former latent use-after-free).  A
            record or array value parameter retains its managed members on
            entry and releases them on scope exit; managed locals are
            null-inited on entry and released on exit alongside plain strings.
            String value parameters and string-typed function results already
            transferred correctly from stages 2 and 3 (the result slot keeps
            its reference for the caller).  Two pre-existing gaps surfaced and
            were fixed: is_string_type now recognizes a record field, an array
            element and a string-returning call, so `rec.field + x` and the
            like lower as string concatenation rather than integer addition
            (it was silently doing pointer arithmetic); and a managed aggregate
            global is 8-byte aligned on LP64 so its pointer members sit on a
            natural boundary, which also lets LeakSanitizer scan them as roots.
            New test pascal_t226_record_string_field covers record copy,
            reassignment, a record value parameter and a local array of
            strings.  Gates: host leak 195/195 and value 195/195, aarch64
            195/195, ColdFire 263/263, full m68k 263/263, compiler ASan
            226/226.
      - [x] Stage 5: in-place self-append.  x := x + e is recognized when x is
            a string l-value and the right side is x + e (same_lvalue matches a
            variable, a field, or an element under a constant or plain-variable
            index).  It lowers to __pascal_str_append, which appends e into x's
            buffer when x is the sole owner with spare capacity and otherwise
            builds a fresh over-allocated string (capacity grows ~1.5x), so a
            build loop amortizes to linear instead of reallocating every
            iteration.  Append consumes x's old reference and returns an owned
            result stored straight back, so no extra retain or release is
            emitted; reading e before growing x keeps x := x + x correct, and a
            shared x is copied (copy-on-write) rather than mutated.  The s[i]
            := c copy-on-write half landed early in stage 2.  New test
            pascal_t227_self_append covers a build loop, self-doubling with an
            aliased operand, and copy-on-write of a shared value.  Gates: host
            leak 196/196 and value 196/196, aarch64 196/196, ColdFire 264/264,
            full m68k 264/264, compiler ASan 227/227.
- [x] P3 Dynamic arrays. `array of T`, SetLength/Length/High, open-array
      parameters, element layout for record elements, refcounted like strings.
      Staged like P2, since kpcom uses dynamic arrays heavily (array of string,
      array of record, array of LongInt/Int64/Boolean):
      - [x] Stage A: scalar-element dynamic arrays.  `array of T` parses (both
            anonymous and named) as a type with op 6; register_type marks it
            is_dynarray with a pointer total_size.  The value is a refcounted
            heap descriptor sharing the string layout ({refcount, kind=2, len,
            cap} header, elements at +PSTR_HDR), so a dynamic array is a
            string-like managed pointer: is_strlike_type_name unifies the two
            for load/store, assignment (retain/release), scope-exit release,
            null-init, value-parameter retain, the managed-member walk and
            pointer globals, while string-only operations (concat, s[i], the
            string builtins) stay keyed on is_string.  Indexing a[i] addresses
            the descriptor pointer + PSTR_HDR + i*elem_size.  Builtins:
            Length/High/Low (on a dynamic array; High/Low also answer a fixed
            array's static bounds), and SetLength, which calls
            __pascal_arr_setlength to resize in place when uniquely owned with
            capacity, else copy into a fresh over-allocated descriptor and drop
            the old reference (copy on write for a shared array); new elements
            zero-filled, SetLength to 0 yields the empty (NULL) array.  Managed
            element types are stage B.  New test pascal_t228_dynarray.  Gates:
            host leak 197/197 and value 197/197, aarch64 197/197, ColdFire
            265/265, full m68k 265/265, compiler ASan 228/228.
      - [x] Stage B: dynamic arrays of managed elements (array of string, array
            of record with string fields).  The element count is only known at
            run time, so freeing or shrinking a descriptor runs a run-time loop
            over a per-element callback.  __pascal_arr_release(a, elsize, dtor)
            finalizes each live element at the last reference before freeing the
            descriptor; __pascal_arr_setlength_mgd(a, newlen, elsize, dtor,
            retain) finalizes the elements a shrink drops and, when copying a
            shared array (copy on write), retains the managed value in each
            copied element so the old and new descriptors each own a reference
            (a uniquely owned reallocation moves the elements, so the old shell
            is freed without finalizing them).  The callbacks: an element that
            is a single managed pointer (array of string, or of a scalar dynamic
            array) uses the fixed runtime __pascal_elem_release_ptr /
            __pascal_elem_retain_ptr; a record element gets a per-type finalize
            (__pdtor_T) and retain (__pretain_T) function generated once from
            emit_managed_walk and appended to the program (gen_managed_elem_fn,
            memoized in elem_fns).  Release routing: emit_managed_walk's release
            leaf and a new emit_strlike_release pick the element-aware arr
            release for a managed-element array, so every release site (string
            store old-value, scope-exit params and locals, record/array member
            walk) is covered; retain of a dynamic array stays a plain descriptor
            retain, since sharing the descriptor shares its elements.  Fixed a
            latent bug the test exposed: a dynamic-array value parameter was
            wrongly treated as a by-address compound to copy (the copy loop
            skipped only is_string, now is_strlike).  New test
            pascal_t229_mgd_dynarray.  Gates: host leak 198/198 and value
            198/198, aarch64 198/198, ColdFire 266/266, full m68k 266/266,
            compiler ASan 229/229.
      - [x] Stage C: open-array parameters and nested dynamic arrays.  An
            `array of T` in a parameter position is an open array: accept_type
            already registers it as an anonymous dynamic-array type, so the
            parameter is a borrowed descriptor-pointer value (a `const` open
            array is neither retained on entry nor released on exit, which the
            op != 0 guard on the retain/release loops already gives), and
            Length/High/Low and indexing work through the Stage A machinery.
            Arguments: a dynamic array or another open array passes its
            descriptor pointer straight through; an array constructor ([] or
            [e, ...]) now builds a descriptor instead of a set when the
            parameter is a dynamic array (lower_open_array_ctor: empty is the
            NULL array; non-empty allocates via __pascal_arr_setlength, stores
            each element at its scalar width, and registers an owned statement
            temp the const parameter borrows).  Nested arrays (array of array of
            T) need no new code: the outer element is a managed pointer (the
            inner descriptor), so Stage B's __pascal_elem_release_ptr finalizes
            each inner array on free or shrink, recursively leak-clean.  Value
            open arrays and array constructors are limited to scalar elements
            (a managed-element constructor is rejected); kpcom only uses `const`
            open arrays of LongInt.  New test pascal_t230_open_array.  Gates:
            host leak 199/199 and value 199/199, aarch64 199/199, ColdFire
            267/267, full m68k 267/267, compiler ASan 230/230.
- [x] P4 Type system. Enum types are now registered, not just their literals.
      The parser emits an N_TYPEDEF (op 7, carrying the member count) alongside
      the per-literal integer constants, and register_type marks it an ordinal
      scalar (is_ordinal, lo_bound 0, hi_bound count-1, 4 bytes); a subrange
      (op 5) is is_ordinal too.  High/Low accept a bare ordinal type name
      (High(TColor)) or an ordinal-typed value and return its static bounds.
      An array index may be an ordinal type, array[TColor] of T: the parser
      allows a dimension with no `..` (the index type in td->b), and because the
      index enum can be forward-referenced, register_type records the index type
      name and the fixup pass resolves the bounds and recomputes the size once
      every type is registered.  A latent trap this surfaced: once an enum type
      is registered, an enum variable assignment (c := cGreen) was taking the
      record/array aggregate-copy path in N_ASSIGN; that path now excludes
      is_scalar types, so an enum, subrange or alias stores its value like any
      ordinal.  `set of E` and `var/field: E` already worked through the set and
      scalar machinery once E is a known 4-byte ordinal (a set stays the 256-bit
      value, fine for enums up to 256 members such as kpcom's ~90-member
      TTokenKind).  Type aliases (op 4) and subrange type declarations (op 5)
      were already present.  Explicit enum ordinals (a = 3) are not implemented;
      kpcom's enums are all implicit 0..n-1.  New test pascal_t231_enum_types.
      Gates: host leak 200/200 and value 200/200, aarch64 200/200, ColdFire
      268/268, full m68k 268/268, compiler ASan 231/231.
- [x] P5 Declarations. `out` parameters, overloading by arity and type,
      record-aggregate typed constants with named fields, const arrays of
      records, function-name-as-var target (Str(v, F)).  Staged; all four are
      exercised by kpcom (out params in ScanNumericByte/ReadWholeFile/
      CollectRoutines/ResolveOverload; a genuine AddMember overload; the
      TokenName/Keywords/NodeKindName typed const arrays; Str(t.Line, line)):
      - [x] Stage A: `out` parameters.  `out` is not reserved, so the parser
            treats a leading `out` before a parameter name as a by-reference
            parameter (same as `var`; the write-only rule is not enforced) and
            otherwise keeps it as an identifier.  Works for scalar, string and
            dynamic-array parameters.  New test pascal_t232_out_params.  Gates:
            host leak/value 201/201, aarch64 201/201, ColdFire 269/269, full
            m68k 269/269, compiler ASan 232/232.
      - [x] Stage B: overloading by arity and type.  Routines may share a name
            with different parameter lists.  At registration each routine gets an
            IR name: a name with a single signature keeps its plain spelling, an
            overloaded name gets a per-signature suffix (name__ovl<k>) that a
            forward declaration and its body share (same_signature groups them,
            so a forward/body pair is one routine, not an overload).  A call site
            resolves by resolve_overload: the parameter count must equal the
            argument count, then a coarse type score (string-like vs real vs
            other) breaks ties and prefers the body over a forward.  lower_function
            now finds the sym for its own declaration (overloads share a name) to
            pick the right ovl name.  A latent gap this surfaced: writeln(f(...))
            where f returns a string was writing the pointer as an integer;
            lower_write_arg now writes a string-returning call's bytes.  Only
            arity-distinct overloading is needed for kpcom (AddMember), but type
            tie-breaking is implemented too.  New test pascal_t233_overload.
            Gates: host leak/value 202/202, aarch64 202/202, ColdFire 270/270,
            full m68k 270/270, compiler ASan 233/233.
      - [x] Stage C: typed constant aggregates with string members.  A scalar
            const array still folds to integer words (flatten_init_list); when
            the constant's type contains a managed string member the global is
            built as a relocatable byte image instead (build_const_image walks
            the initializer against the type: a string leaf becomes a pointer to
            a static strobj via intern_strobj, with '' as NULL; an array walks
            elements at base + i*elem_size; a record walks fields at their
            offsets, the initializer listing values in declared field order).
            The image rides the existing ir_global `inits` list (offset, size,
            ival or sym), already emitted with symbol relocations by every
            backend.  Covers kpcom's TokenName/NodeKindName (array[enum] of
            string) and Keywords (array of record).  An array bound that is a
            forward-referenced const, array[1..Count], is deferred: register_type
            evaluates a bound immediately only when every name in it is an
            already-registered const (bound_resolvable), else stores the bound
            expressions and the fixup pass resolves them once the whole const
            section is registered.  New test pascal_t234_typed_const.  Gates:
            host leak/value 203/203, aarch64 203/203, ColdFire 271/271, full
            m68k 271/271, compiler ASan 234/234.
      - [x] Stage D: Str(v, F) with a function-name destination.  Str already
            converted a value to a string and stored it in a string variable;
            the destination may now also be the enclosing function's string
            result (Str(v, F) inside function F stores to its result slot), and
            the l-value check is by lvalue_type_name so a string field or element
            works too.  Two latent write gaps this surfaced, both fixed in
            lower_write_arg: writeln of a string-returning call, and of a
            string-valued concatenation (a + b), were writing the pointer as an
            integer; a new node_is_string_valued predicate routes any
            string-valued expression to __pascal_write_str.  New test
            pascal_t235_str_dest.  Gates: host leak/value 204/204, aarch64
            204/204, ColdFire 272/272, full m68k 272/272, compiler ASan
            235/235.
- [x] P6 Externals and host imports. `external cdecl` declarations. The host
      runtime imports: ReadFileStr, WriteFileStr, kpd_open/next/close,
      ParamStr/ParamCount, ErrOutput, Assigned. {$I file} include splicing in
      expression position. WriteLn(ErrOutput, ...).  Staged (Assigned is not
      used by kpcom, so it is skipped):
      - [x] Stage A: `external cdecl` declarations.  A proc/func header followed
            by `external [cdecl];` has no body (parsed like forward, skipped in
            lowering).  The symbol links by the exact source spelling, not the
            lowercased identifier, because the host runtime exports CamelCase
            names (ReadFileStr); the lexer now keeps each identifier's original
            case (token.raw -> node.raw_name) and an external routine's sym uses
            it as the IR name.  cdecl is the calling convention the compiler
            already emits for an external call, so the word is accepted and
            ignored.  A string-returning import uses the owned-result ABI, the
            shape of ReadFileStr.  New test pascal_t236_external.  Gates: host
            leak/value 205/205, aarch64 205/205, ColdFire 273/273, full m68k
            273/273, compiler ASan 236/236.
      - [x] Stage B: Write/WriteLn stream selection and Flush.  A leading
            Output or ErrOutput argument to Write/WriteLn selects the output
            stream: the runtime keeps a current output fd (g_outfd, default 1),
            __pascal_set_outfd changes it, and all the write helpers and the new
            __pascal_write_raw (which write_str_literal and the newline now use
            instead of a hardcoded fd 1) honor it; the compiler brackets an
            ErrOutput write with set_outfd(2)/set_outfd(1).  Output is unbuffered
            (direct write), so Flush is recognized as a no-op builtin.  New
            host-only test pascal_t237_streams_hostio (the _hostio suffix marks a
            test needing stdout and stderr kept separate, which the host harness
            does but the VM harnesses merge with 2>&1, so it is excluded from the
            ColdFire and m68k runs like the _real float tests).  Gates: host
            leak/value 206/206, aarch64 206/206, ColdFire 273/273, full m68k
            273/273, compiler ASan 237/237.
      - [x] Stage C: {$I file} include splicing in expression position.  The
            lexer gained an include stack: the $I (or $INCLUDE) directive reads
            the named file (resolved relative to the current source file's
            directory, then as given), pushes the current input position, and
            switches to the file's text; at the included text's end skip_ws pops
            back to the parent.  Because the splice happens in the lexer, an
            include works anywhere, including an expression such as a constant's
            value (KP_VERSION = {$I version.inc}).  New test
            pascal_t238_include with fixtures t238_version.inc / t238_count.inc.
            Gates: host leak/value 207/207, aarch64 207/207, ColdFire 274/274,
            full m68k 274/274, compiler ASan 238/238.
      - [x] Stage D: ParamStr/ParamCount.  main now takes (argc, argv) and calls
            __pascal_set_args at entry to record them in the runtime; a
            freestanding host that passes none leaves them zero and unused.
            ParamCount (argc-1, usable bare or called) and ParamStr(i) (argument
            i as an owned heap string, 0 the program name, out-of-range empty)
            lower to runtime calls.  A related gap was closed so writeln and
            Length work on a string-returning builtin call: is_string_type now
            recognizes copy/paramstr/concat.  New host-only test
            pascal_t239_params_hostio.  Gates: host leak/value 208/208, aarch64
            208/208, ColdFire 274/274, full m68k 274/274, compiler ASan
            239/239.
- [x] P7 Compile kpcom.pas with KOBOLD defined, link the native kpcom, and add
      it as an acceptance test. Close any remaining gaps the real source finds.

      Done. kpcom.pas (~12.8k lines) compiles with the skj-pc front end (KOBOLD
      defined, so its file I/O is external cdecl), links against the host
      runtime and the host-import shims (tests/pascal_kpcom_host.c), and runs:
      --help, --dump-tokens, --dump-ast, and --emit-kpm all work on real Kobold
      sources. The acceptance target `make check-pc-kpcom` builds kpcom this way
      and checks that it emits byte-identical KPM to the reference front end
      (build/skj-kp-jit) over the whole behavioral corpus: all 86 of 86 modules
      match. It prints the kpcom build wall time (the long pole, dominated by
      NASM assembling the ~2.6 MB module) as a `pc-kpcom: build ...` metric line
      with a per-stage breakdown.

      Gaps closed (each a genuine codegen bug the real source exposed):
      - Val now accepts any lvalue destination (name, field, element) and
        dispatches by result type to __pascal_val / _int64 / _real.
      - a string-valued constant indexes like a string (canonical $string type).
      - a set expression (constructor or operator) passes to a set value
        parameter by materializing it and passing its address.
      - a function whose result is a record returns it through a hidden sret
        pointer; the function name as an l-value, r-value, or string self-append
        target reaches the result slot.
      - enum/subrange/alias parameters pass by value, not by address.
      - nil is the null pointer (empty string / dynamic-array handle).
      - inc/dec accept a field or element l-value, not only a plain variable.
      - a char value (var, field, element, or char-returning call) promotes to
        a one-character string when concatenated (__pascal_str_ofchar).
      - boolean and/or short-circuit (FPC default {$B-}).
      - a nested concatenation is recognized as a string value, so
        `a + b + ' ' + c` stays string concatenation throughout.
      - call arguments evaluate right to left (FPC order).
      - a nested function gets its own slot-size array, so it cannot corrupt
        its parent's frame; integer `not` is bitwise, not logical.
      - `a[i] := rhs` evaluates rhs before taking the destination address, so a
        rhs call that reallocates the indexed array cannot leave the store
        pointing into the freed buffer (kpcom's `Nodes[p].A := ParseBlock`).
      - Exit(v) sets the function result through the same path as a result
        assignment, retaining a borrowed string and storing reals and int64s at
        their own width, rather than a plain word store that dropped a borrowed
        string's reference (kpcom's FindMethodName returning a shared name).

Status notes live under each phase as it lands.
