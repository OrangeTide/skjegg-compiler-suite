unit curl;
{ A Kobold binding for libcurl's easy interface: fetch a URL and get its body back
  as a string. It is an optional native-backed unit, built only when ./configure
  found libcurl (HAVE_LIBCURL); its C glue (kobold/lib/curl_glue.c) carries the
  write-callback shim that libcurl delivers the body through, and the -lcurl link
  flag rides in the curl.link sidecar. See doc/languages/kobold/kp-native-units.md.

  Kobold holds no libcurl handle. Get runs a whole transfer and returns the body;
  the status of that last transfer is read back with LastStatus and LastOk. Any
  scheme libcurl supports works, so a `file://` URL reads a local file through the
  same path as an `http://` fetch. This is AOT only; the JIT cannot reach an
  external library's symbols. }
interface

{ Fetch a URL and return its body. The empty string on a transfer failure; check
  LastOk to tell an empty body from a failure. Redirects are followed. }
function Get(const url: string): string;

{ The HTTP status code of the last Get (for example 200 or 404), or 0 for a scheme
  with no status, such as file://. }
function LastStatus: integer;

{ Whether the last Get completed its transfer (libcurl returned success). A 404 is
  still a completed transfer, so a caller distinguishes "fetched, got an error
  page" from "could not fetch" with LastStatus. }
function LastOk: boolean;

implementation

{ The C glue (kobold/lib/curl_glue.c), declared once here; resolved at link from the
  object archived in curl.a, with -lcurl from the unit's .link sidecar. }
function kpcurl_get(const url: string): string; cdecl; external;
function kpcurl_last_status: integer; cdecl; external;
function kpcurl_last_ok: integer; cdecl; external;

function Get(const url: string): string;
begin
  Get := kpcurl_get(url);
end;

function LastStatus: integer;
begin
  LastStatus := kpcurl_last_status;
end;

function LastOk: boolean;
begin
  LastOk := kpcurl_last_ok <> 0;
end;

end.
