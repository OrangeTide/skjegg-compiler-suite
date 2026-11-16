unit netchan;
{ A Kobold binding for netchan, a multiplexed reliable/unreliable channel protocol,
  over UDP. It is an optional native-backed unit; its C glue
  (kobold/lib/netchan_glue.c) wraps the vendored netchan core
  (third_party/netchan/) and owns the UDP socket and the clock, since netchan
  itself is transport-agnostic. See doc/languages/kobold/kp-native-units.md.

  A connection and a channel are each an integer id (> 0 on success, 0 on failure),
  never a pointer. OpenServer and OpenClient create a connection; Service runs the
  socket pump and reports the connection state, so a program drives the link by
  calling Service in its loop. A reliable or unreliable channel carries datagrams:
  the sender opens it with OpenChannel and Write; the receiver learns of delivered
  data from Poll (an EvData event) and takes it with Read. This is AOT only; the JIT
  cannot reach an external library's symbols. }
interface

const
  { Channel kinds (the `type` of OpenChannel). }
  Reliable   = 0;      { ordered, acked, retransmitted datagrams }
  Unreliable = 1;      { fire-and-forget datagrams }
  Stream     = 2;      { a byte stream }

  { Channel direction (the `dir` of OpenChannel). }
  DirSend = 0;
  DirRecv = 1;

  { Connection states, as returned by Service and State. }
  StateNew        = 0;
  StateConnecting = 1;
  StateConnected  = 2;
  StateClosing    = 3;
  StateClosed     = 4;

  { Event kinds, as returned by Poll. }
  EvNone        = 0;
  EvConnected   = 1;
  EvDisconnected = 2;
  EvRedirect    = 3;
  EvChanOpen    = 4;
  EvChanClose   = 5;
  EvData        = 6;

{ Open a server bound to a UDP port (0 picks an ephemeral port, which Port then
  reports), or a client connecting to host:port. A connection id > 0, or 0 on
  failure. }
function OpenServer(port: integer): integer;
function OpenClient(const host: string; port: integer): integer;

{ The local UDP port the connection is bound to, in host byte order. 0 for an
  invalid id. }
function Port(conn: integer): integer;

{ Run one pump step (send queued packets, receive pending ones, service timers,
  and on a server accept a pending handshake) and return the connection state.
  Call it in a loop to drive the connection. }
function Service(conn: integer): integer;

{ The connection state without pumping. }
function State(conn: integer): integer;

{ Drain one event from the connection, returning its kind (EvNone when the queue is
  empty). After an EvData the delivered datagram is taken with Read. }
function Poll(conn: integer): integer;

{ Open a channel on a connection (both peers open the same layout in the same
  order). A channel id > 0, or 0 on failure. }
function OpenChannel(conn, kind, dir: integer; const contentType: string): integer;

{ Queue one datagram on a channel. The byte count written, or a negative error. }
function Send(channel: integer; const data: string): integer;

{ Take the next datagram delivered on the connection's most recent data channel
  (the one named by the last EvData), as a string; the empty string when none is
  ready. }
function Recv(conn: integer): string;

{ Close a connection and its socket. }
procedure Close(conn: integer);

implementation

{ The C glue (kobold/lib/netchan_glue.c), declared once here; resolved at link from
  the object archived in netchan.a. }
function kpnc_open_server(port: integer): integer; cdecl; external;
function kpnc_open_client(const host: string; port: integer): integer; cdecl; external;
function kpnc_port(id: integer): integer; cdecl; external;
function kpnc_service(id: integer): integer; cdecl; external;
function kpnc_state(id: integer): integer; cdecl; external;
function kpnc_poll(id: integer): integer; cdecl; external;
function kpnc_open_channel(id, kind, dir: integer; const contentType: string): integer; cdecl; external;
function kpnc_write(channel: integer; const data: string): integer; cdecl; external;
function kpnc_read(id: integer): string; cdecl; external;
procedure kpnc_close(id: integer); cdecl; external;

function OpenServer(port: integer): integer;
begin
  OpenServer := kpnc_open_server(port);
end;

function OpenClient(const host: string; port: integer): integer;
begin
  OpenClient := kpnc_open_client(host, port);
end;

function Port(conn: integer): integer;
begin
  Port := kpnc_port(conn);
end;

function Service(conn: integer): integer;
begin
  Service := kpnc_service(conn);
end;

function State(conn: integer): integer;
begin
  State := kpnc_state(conn);
end;

function Poll(conn: integer): integer;
begin
  Poll := kpnc_poll(conn);
end;

function OpenChannel(conn, kind, dir: integer; const contentType: string): integer;
begin
  OpenChannel := kpnc_open_channel(conn, kind, dir, contentType);
end;

function Send(channel: integer; const data: string): integer;
begin
  Send := kpnc_write(channel, data);
end;

function Recv(conn: integer): string;
begin
  Recv := kpnc_read(conn);
end;

procedure Close(conn: integer);
begin
  kpnc_close(conn);
end;

end.
