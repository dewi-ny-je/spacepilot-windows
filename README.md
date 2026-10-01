# Axial for Windows

An open-source 3Dconnexion SpaceMouse, SpacePilot and SpaceExplorer driver for
Windows 11, ported from [Axial](https://github.com/consi/axial) for macOS. Axial
provides six-axis navigation through the same compatibility APIs that
applications such as Autodesk Fusion, PrusaSlicer, FreeCAD and Blender use with
3Dconnexion's 3DxWare, without depending on the vendor driver.

## Install

Download `Axial-<version>-x64-setup.exe` from the CI artifacts or a release and
run it. Uninstall 3DxWare first, and quit CAD applications before installing or
upgrading: the installer replaces `siappdll.dll` and `TDxNavLib.dll` in
`System32` (64-bit) and `SysWOW64` (32-bit), and asks for a restart when a
running application still holds them.

After installation:

1. Axial opens its settings window and starts its input service. Closing the
   window keeps Axial in the notification area; **Quit Axial** stops both.
   **Start at login** starts Axial hidden when you sign in.
2. Keyboard shortcuts are sent with `SendInput`. Windows does not let a normal
   process send input to an elevated (administrator) window, so shortcuts do not
   reach elevated applications.
3. Reopen your CAD application. In Fusion select the **Latest** SpaceMouse driver.

Uninstalling from **Settings → Apps** removes the program, the compatibility
DLLs, the login item and Axial's web certificate. Saved profiles in
`%LOCALAPPDATA%\Axial` are retained.

The installer and binaries are not code-signed, so SmartScreen may warn the first
time you run them.

## Supported APIs

| API | Access | Purpose |
| --- | --- | --- |
| 3Dconnexion Client API | `siappdll.dll` (`SiInitialize`, `SiOpen`, `SiGetEvent`, …) | Applications built with the 3DxWare SDK's legacy `siapp` interface |
| Navlib API | `TDxNavLib.dll` (`NlCreate`, `NlReadValue`, `NlWriteValue`, …) | Applications using the 3Dconnexion navigation library (Fusion, FreeCAD, …) |
| 3DconnexionJS API | HTTPS discovery at `https://127.51.68.120:8181/3dconnexion/nlproxy`; WAMP 1.0 at `wss://127.51.68.120:8181/` | Browser-based 3D navigation compatible with 3DconnexionJS (Onshape and others) |
| Event stream | Unix socket at `%LOCALAPPDATA%\Axial\events` | Subscribe to device, motion and button events, including from a background monitor, or inject events in mock mode; see [the event stream API](docs/events.md) |
| Control API | Unix socket at `%LOCALAPPDATA%\Axial\events.control` | Query status and configuration, update configuration, publish command catalogs, or stop the service; `axialctl` uses it |

`AXIAL_SOCKET` relocates both sockets and `AXIAL_DATA` relocates the whole data
directory. The socket APIs are local to the signed-in user: the service rejects
connections from processes running as another Windows user.

Web navigation is enabled by default. In **Service & diagnostics → WebSocket API
compatibility**, click **Set Up…** and approve the Windows security prompt that
asks to trust the local Axial certificate authority. The CA is name-constrained
to `127.51.68.120`, is trusted only for your user, and its private key is
discarded after it signs the server certificate. Axial checks setup health every
minute and offers renewal when needed. No separate OpenSSL installation is
required; quitting Axial closes the web listener. `127.51.68.120` is already a
loopback address on Windows, so no network configuration changes are made.

## Build

Requirements: Visual Studio 2022 with the C++ desktop workload, CMake 3.25+,
the .NET 8 SDK, OpenSSL 3.5 built as static `/MT` libraries (see
[CONTRIBUTING.md](CONTRIBUTING.md)) and Inno Setup 6 for the installer.

```powershell
cmake --preset x64 -DAXIAL_OPENSSL_ROOT=C:\axial-tls\x64
cmake --build --preset x64
ctest --preset x64
dotnet test app/Axial.Core.Tests -p:AxialBridgeDirectory=$PWD\build\x64\bin\Release
dotnet publish app/Axial/Axial.csproj -c Release -r win-x64 --self-contained -o out/app
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the 32-bit adapters, the installer,
the Linux/Wine cross build and the layout of the repository.

## Supported devices

Axial recognizes these USB device identities, including the SpaceMouse Wireless
through its Universal Receiver. Bluetooth connections are untested on Windows. Counts
refer to physical HID controls exposed in Buttons.

| Device | USB vendor:product | Buttons |
| --- | --- | ---: |
| Spaceball 5000 USB | `046d:c621` | 12 |
| SpaceTraveler | `046d:c623` | 8 |
| SpacePilot | `046d:c625` | 21 |
| SpaceNavigator | `046d:c626` | 2 |
| SpaceExplorer | `046d:c627` | 15 |
| SpaceNavigator for Notebooks | `046d:c628` | 2 |
| SpacePilot Pro | `046d:c629` | 21 primary controls¹ |
| SpaceMouse Pro | `046d:c62b` | 15 |
| SpaceMouse Wireless (USB) | `256f:c62e` | 2 |
| SpaceMouse Pro Wireless (USB) | `256f:c631` | 15 |
| SpaceMouse Enterprise | `256f:c633` | 31 |
| SpaceMouse Compact | `256f:c635` | 2 |
| SpaceMouse Module | `256f:c636` | 2 inputs |
| SpaceMouse Pro Wireless BT (USB) | `256f:c638` | 15 |
| SpaceMouse Wireless (Universal Receiver) | `256f:c652` | 2 |

¹ SpacePilot Pro shares physical keys between primary and alternate codes; LCD
controls are not implemented. Enterprise's extended buttons have synthetic
report coverage. LED support depends on the device's HID output descriptor.

## Credits

Axial builds on the work and research of these open-source projects:

| Project | Contribution to Axial |
| --- | --- |
| [FreeSpacenav / spacenavd](https://github.com/FreeSpacenav/spacenavd) | USB device identities and protocol/button-mapping references |
| [Axial](https://github.com/consi/axial) | The macOS driver this port follows: service, device catalog, compatibility layers, settings app design and test suite (MIT) |
| [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) | Client-framework integration used to define compatibility tests |
| [FreeCAD](https://github.com/FreeCAD/FreeCAD) | Public Navlib interface declarations, retained under LGPL-2.1 terms |
| [Blender](https://github.com/blender/blender) | Native SpaceMouse button maps used as hardware references |
| [PySpaceMouse](https://github.com/JakubAndrysek/PySpaceMouse) | MIT device definitions used by native button-replay tests |
| [nytamin/spacemouse](https://github.com/nytamin/spacemouse) | MIT raw-HID test cases adapted into native regression tests |
| [ANTz](https://github.com/openantz/antz) | Enterprise HID report investigation |
| [3dxdisp-pro](https://github.com/MiguelDLM/3dxdisp-pro) | SpacePilot Pro hardware and button research |
| [Boost](https://www.boost.org/) | Asio, Beast and JSON (Boost Software License 1.0) |
| [OpenSSL](https://www.openssl.org/) | TLS for the local 3DconnexionJS server (Apache License 2.0) |
| [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ToyCar) | CC0 Toy Car model by Guido Odendahl, with materials and scene by Eric Chadwick |

## Trademarks

3Dconnexion, SpaceMouse, SpaceExplorer, SpacePilot, SpaceNavigator,
SpaceTraveler, SpaceBall, and the other 3Dconnexion product names referenced in
this README are trademarks or registered trademarks of 3Dconnexion. Axial is
independent of and not endorsed by 3Dconnexion.

## License

Axial's original code is [MIT licensed](LICENSE). Third-party material retains
its own license; see the notices alongside each dependency. Axial is independent
of 3Dconnexion, Autodesk and Prusa Research. Proprietary driver binaries and
installed proprietary SDK files are not included.
