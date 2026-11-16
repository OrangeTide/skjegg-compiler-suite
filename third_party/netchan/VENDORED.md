# Vendored netchan

- **Version:** 0.10.0 (git tag `v0.10.0`)
- **Upstream:** https://github.com/OrangeTide/netchan/
- **Layer:** the protocol core (`src/`) plus the UDP transport
  (`transport/nc_udp.*`), the "protocol over UDP" row of the upstream
  `VENDORING.md`.
- **Files:** `netchan.c`, `netchan.h`, `nc_addr.h` (the core), `nc_udp.c`,
  `nc_udp.h` (the UDP address packing), copied flat so a single
  `-Ithird_party/netchan` resolves every plain-name include. `LICENSE` is the
  upstream license file.
- **License:** public domain (CC0-1.0); no attribution required.
- **Local changes:** none.

The `stb_truetype`-style `stb_image` units vendor a single header; netchan is a
small multi-file library the project built to be copied in this way (it has no
`pkg-config` and no shared library). The `kobold/lib/netchan.pas` unit binds the
UDP layer: its glue (`kobold/lib/netchan_glue.c`) owns a UDP socket and drives
the transport-agnostic core's feed / send / service pump, since the core leaves
the socket and the clock to the application.

To upgrade, re-copy the five files from a newer tag (the upstream `tools/vendor.sh
--layer transport/nc_udp` does the same) and run `make check-kp-netchan`.
