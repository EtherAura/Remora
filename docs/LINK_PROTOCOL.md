# The link protocol (v1)

How a paired phone reaches this computer: the transport the phone app speaks before protocol v3
([MIRROR_AGENT.md](MIRROR_AGENT.md), [MIRROR_PROTOCOL.md](MIRROR_PROTOCOL.md)) takes over. The
decision behind it is [PHONE_APP.md](PHONE_APP.md) §2. `src/core/LinkProto.{h,cpp}`
implements the formats and `tests/test_link.cpp` pins them, including a real TLS session against
the server in `src/engine/Link.cpp`. All integers are big-endian.

## Trust

Both sides present a self-signed certificate, and neither trusts a CA: each pins the other's
**SHA-256 fingerprint of the DER certificate**, written as 64 lowercase hex digits.

- The computer's key and certificate are made on first use (ECDSA P-256) and kept in
  `$XDG_DATA_HOME/remora/link/`. The phone pins the fingerprint it was given when pairing and
  refuses any other.
- The phone presents its own certificate on every connection. The computer accepts a request
  other than *pair* only from a certificate whose fingerprint it has paired.

TLS 1.3 only.

## Pairing

`remora link pair` makes a one-time token, good for ten minutes and stored only as its hash, and
shows it as a URI, and as a QR code when `qrencode` is installed:

```
remora-link://pair?h=192.168.0.10:27200&h=[fd00::1]:27200&fp=<fingerprint>&t=<token>&n=<host name>
```

`h` repeats, once per address the computer can be reached on (LAN and VPN interfaces; IPv6
bracketed), with the port. The phone tries them in order, pins `fp`, and sends a *pair* request
with `t`. A token works once; a new `pair` replaces a pending one.

## Requests

Every TLS connection carries exactly one request:

```
"RMLK"  u16 version (1)  u8 kind   then the kind's fields, each a u16-prefixed UTF-8 string
  1 pair      token, device name
  2 sources   —
  3 connect   source id
```

and one reply:

```
u8 status (0 ok)  u32 length  payload
```

| request | ok payload | after the reply |
|---|---|---|
| pair | the name the device was saved under (a number is added to a name already taken) | closed |
| sources | a JSON array of `{"id", "name", "kind"}` | closed |
| connect | empty | **the connection is the source's**: bytes pass both ways untouched |

A refusal's payload is a UTF-8 reason fit to show the person, and the connection closes after it.
A request with the wrong magic, another version or an unknown kind is refused the same way.

## Sources

| kind | id | what is behind it |
|---|---|---|
| `computer` | `computer` | `remora-host-agent serve`: this computer's own apps |
| `android` | `android:<profile>` | that Remora profile's in-image agent, through an adb forward |

The list holds only what can be opened now: the computer while its agent serves, and each profile
whose device answers adb.

After a *connect*, the phone speaks protocol v3 as if it had reached the agent directly: the
`RMRA` hello, the role byte, the session request or attach. Each of a session's connections
(control, video, audio) is its own TLS connection with its own *connect*. A phone may send its
hello right behind the *connect*; it is delivered once the source is reached.
