# Event stream API

Axial provides a local Unix domain socket that carries device events as fixed
size binary records. It is useful for diagnostics, integrations, and tools that
need direct access to SpaceMouse input without loading one of Axial's
compatibility DLLs.

## Connection

The default socket path is:

```text
%LOCALAPPDATA%\Axial\events
```

Windows 10 version 1803 and later support `AF_UNIX` stream sockets. Set
`AXIAL_SOCKET` to use another path; set `AXIAL_DATA` to move the whole data
directory (profiles, sockets and logs). The service restricts the data directory
to the logged-in user and SYSTEM, and it rejects connections from processes
running as another Windows user. The socket is an `AF_UNIX` `SOCK_STREAM`. It
has no message boundaries, so clients must read and write complete records
themselves.

The service may be unavailable while Axial is starting or after it has stopped.
Clients should reconnect when a connection fails. A client that uses the
`Stream` helper in `include/axial/stream.hpp` is automatically retried every
100 ms.

## Record format

Every record is exactly 64 bytes. Values use the host's little-endian layout on
Windows x64 and x86. The layout is:

| Offset | Size | Field | Description |
| ---: | ---: | --- | --- |
| 0 | 4 | `magicValue` | `0x534e4156` (`SNAV`) |
| 4 | 2 | `version` | `1` |
| 6 | 2 | `kind` | Event kind, described below |
| 8 | 8 | `received` | Monotonic timestamp in nanoseconds |
| 16 | 8 | `sequence` | Per-device input sequence number |
| 24 | 4 | `device` | Axial device identifier |
| 28 | 4 | `buttons` | Current 32-bit button mask |
| 32 | 12 | `axes` | Six signed 16-bit axis values |
| 44 | 2 | `vendor` | USB vendor ID |
| 46 | 2 | `product` | USB product ID |
| 48 | 4 | `flags` | Event and connection flags |
| 52 | 4 | `pid` | Sender process ID in a hello record |
| 56 | 8 | `decoded` | Service decode timestamp in nanoseconds |

`received` and `decoded` use `QueryPerformanceCounter` converted to nanoseconds.
They are suitable for ordering and latency measurements within the running
system; they are not wall-clock timestamps.

## Opening a stream

The first record sent by a client must be a `hello` record. The service sends
the current `added` record for each connected device after accepting a normal
subscriber. A hello record normally has `pid` set to the client's process ID.

Set these flags in the hello record to select a stream:

| Flag | Value | Meaning |
| --- | ---: | --- |
| `monitor` | `1` | Receive raw events before the active application's profile filtering |
| `replay` | `2` | Send events into the service instead of subscribing; accepted only by the mock service used for tests |

With no flags, a subscriber receives events after the active profile has applied
gain, dead zones, axis inversion, dominant-axis mode, translation/rotation
switches, orbit mode, and suppressed buttons. Motion and button events are
delivered to the foreground application. Device lifecycle events are delivered
regardless of foreground focus. A monitor receives the raw event data and does
not receive profile-filtered data. Monitor events are also independent of
foreground focus: a monitoring app can remain in the background and continue to
observe SpaceMouse motion and button activity while another app is active.

The service identifies the peer process and requires it to run as the same Windows
user (SID) as the service. A malformed record, an invalid first record, or a full
client queue closes the connection.

## Event kinds

| `kind` | Value | Meaning |
| --- | ---: | --- |
| `hello` | 1 | Client registration record; sent by the client only |
| `motion` | 2 | One or more of the six axes changed; `axes` contains the current values |
| `buttons` | 3 | The button mask changed; `buttons` contains the current mask |
| `added` | 4 | A device became available; identity fields are populated |
| `removed` | 5 | A device was disconnected; `axes` and `buttons` are cleared |
| `reset` | 6 | Input state was reset, such as after focus or service changes |
| `command` | 7 | An application command was triggered; `flags` identifies the command |

`device` is zero for service-wide reset events. `vendor` and `product` contain
the USB identity for device events. A button bit is set while that button is
held. Axis values are signed HID-scale values; their interpretation as
translation or rotation depends on the consumer and the active profile.

A `command` record is sent to the foreground subscriber when a button assigned
to a driver command is pressed. `flags` is `0x10000` to fit the model in the
view, or `0x20000` for a standard view with the view number in the top byte:
1 front, 2 back, 3 left, 4 right, 5 top, 6 bottom, 7 isometric from front,
right and top, 8 isometric from front, left and top. A client shows that view
and fits the model in it.

## Use it from Python

In practice, a client opens the Unix socket, sends a `hello` record, then reads
64-byte records until the service closes the connection. Python's standard
library is sufficient; no Axial package is required. The interpreter's
`socket` module must expose `AF_UNIX` on Windows; builds that do not raise
`AttributeError` at `socket.AF_UNIX`, and PowerShell 7 (below) works instead. This complete example
runs as a background monitor and prints useful information for every event:

```python
import os
import socket
import struct

MAGIC = 0x534E4156
VERSION = 1
HELLO, MOTION, BUTTONS, ADDED, REMOVED, RESET, COMMAND = range(1, 8)
MONITOR = 1

# < means little-endian. The format is exactly 64 bytes.
EVENT = struct.Struct("<IHHQQII6hHHIIQ")


def read_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)


def socket_path():
    default = os.path.join(os.environ["LOCALAPPDATA"], "Axial", "events")
    return os.environ.get("AXIAL_SOCKET", default)


def connect_monitor():
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.connect(socket_path())
    hello = EVENT.pack(
        MAGIC, VERSION, HELLO,
        0, 0, 0, 0,                 # received, sequence, device, buttons
        0, 0, 0, 0, 0, 0,            # six axes
        0, 0,                       # vendor, product
        MONITOR, os.getpid(), 0,     # flags, pid, decoded
    )
    connection.sendall(hello)
    return connection


def unpack_event(record):
    values = EVENT.unpack(record)
    if values[0] != MAGIC or values[1] != VERSION:
        raise RuntimeError("unsupported Axial event record")
    return {
        "kind": values[2], "received": values[3], "sequence": values[4],
        "device": values[5], "buttons": values[6], "axes": values[7:13],
        "vendor": values[13], "product": values[14], "flags": values[15],
        "pid": values[16], "decoded": values[17],
    }


devices = {}
with connect_monitor() as connection:
    while record := read_exact(connection, EVENT.size):
        event = unpack_event(record)
        kind = event["kind"]
        device = event["device"]

        if kind == MOTION:
            devices.setdefault(device, {})["axes"] = event["axes"]
            print("motion", device, event["axes"])
        elif kind == BUTTONS:
            devices.setdefault(device, {})["buttons"] = event["buttons"]
            print("buttons", device, hex(event["buttons"]))
        elif kind == ADDED:
            devices[device] = {
                "vendor": event["vendor"], "product": event["product"],
                "axes": event["axes"], "buttons": event["buttons"],
            }
            print("added", device, hex(event["vendor"]), hex(event["product"]))
        elif kind == REMOVED:
            devices.pop(device, None)
            print("removed", device)
        elif kind == RESET:
            if device == 0:
                devices.clear()
            elif device in devices:
                devices[device]["axes"] = (0, 0, 0, 0, 0, 0)
                devices[device]["buttons"] = 0
            print("reset", device)
        elif kind == COMMAND:
            print("command", device, hex(event["flags"]))
        elif kind == HELLO:
            raise RuntimeError("hello is sent by the client, not received")
        else:
            raise RuntimeError(f"unknown event kind: {kind}")
```

Save it as `monitor.py` and run `py monitor.py` while Axial is running.
The first records are normally `added` events for devices that are already
connected. The monitor continues receiving motion and button events while the
monitoring process stays in the background and another app is active.

The handler keeps the latest state in `devices`: `motion` replaces the six axis
values, `buttons` replaces the button mask, `added` creates a device entry, and
`removed` deletes one. `reset` clears transient state; a reset with `device ==
0` applies to all devices. `command` is an application command and should be
interpreted from `flags`; `hello` is sent during setup and is not expected in
the receive loop.

`recv()` can return a partial record because this is a stream socket, so the
`read_exact` helper is required. When it returns `None`, the service has closed
the connection. A long-running integration should close the socket, wait
briefly, and call `connect_monitor()` again.

To receive profile-filtered events for the foreground application, send a
hello with `flags` set to `0` instead of `MONITOR`. Set `AXIAL_SOCKET` when the
service uses a non-default path; otherwise the example derives the default
path from `%LOCALAPPDATA%`.

## Use it from PowerShell 7

PowerShell 7 runs on .NET, whose sockets support `AF_UNIX` on every Windows
version Axial supports. This monitor prints the kind, device, buttons and axes of
each record:

```powershell
$path = $env:AXIAL_SOCKET ?? (Join-Path $env:LOCALAPPDATA 'Axial\events')
$socket = [Net.Sockets.Socket]::new('Unix', 'Stream', 'Unspecified')
$socket.Connect([Net.Sockets.UnixDomainSocketEndPoint]::new($path))
$hello = [byte[]]::new(64)
[BitConverter]::GetBytes([uint32]0x534E4156).CopyTo($hello, 0)
[BitConverter]::GetBytes([uint16]1).CopyTo($hello, 4)   # version
[BitConverter]::GetBytes([uint16]1).CopyTo($hello, 6)   # hello
[BitConverter]::GetBytes([uint32]1).CopyTo($hello, 48)  # monitor flag
[BitConverter]::GetBytes([uint32]$PID).CopyTo($hello, 52)
[void]$socket.Send($hello)
$record = [byte[]]::new(64)
while ($true) {
    $read = 0
    while ($read -lt 64) {
        $n = $socket.Receive($record, $read, 64 - $read, 'None')
        if ($n -eq 0) { return }
        $read += $n
    }
    $axes = 0..5 | ForEach-Object { [BitConverter]::ToInt16($record, 32 + 2 * $_) }
    '{0} device={1} buttons=0x{2:x} axes={3}' -f [BitConverter]::ToUInt16($record, 6),
        [BitConverter]::ToUInt32($record, 24), [BitConverter]::ToUInt32($record, 28), ($axes -join ',')
}
```

## Related control socket

The service's request API is a separate newline-delimited JSON protocol at
`%LOCALAPPDATA%\Axial\events.control` (or the `AXIAL_SOCKET` path with
`.control` appended). The bundled `axialctl` command uses it for `status`,
`config`, `commands`, `calibrate`, and configuration updates. `{"op":"calibrate"}`
makes each device's current deflection its rest position (add `"device":id` for
one device, or `"clear":true` to undo); event records and status then report
axes relative to it. It is not part of the binary
event record stream described here.
