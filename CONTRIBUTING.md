# Contributing to Axial for Windows

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Input service, `siappdll.dll`, `TDxNavLib.dll`, web server and setup tool, `axialctl`, settings bridge DLL |
| `include/axial/` | Shared HID decoder, device catalog, event transport, platform layer and navigation logic |
| `app/Axial.Core/` | Settings app model (C#, `net8.0`): configuration, service client, diagnostics, web setup, logs |
| `app/Axial.Core.Tests/` | xUnit port of axial's Swift app tests; runs on Windows and Linux |
| `app/Axial/` | WPF settings app and notification-area icon |
| `tests/` | Native C++ regression tests ported from axial, mock service and fixtures |
| `cmake/` | Dependencies (Boost, OpenSSL), MinGW toolchain and test definitions |
| `installer/` | Inno Setup script |
| `third_party/` | Pinned interface declarations, the Toy Car model and license notices |

Build outputs belong under `build/` and `out/`, which are ignored.

## Native build and tests

Use a *Developer PowerShell for VS 2022* (x64). OpenSSL is built once as static
libraries with the static CRT, exactly as CI does:

```powershell
# OpenSSL 3.5.8, from the release tarball, with Strawberry Perl
perl Configure VC-WIN64A no-shared no-tests no-module no-asm --prefix=C:\axial-tls\x64 --openssldir=C:\axial-tls\x64\ssl
nmake build_libs
nmake install_dev

cmake --preset x64 -DAXIAL_OPENSSL_ROOT=C:\axial-tls\x64
cmake --build --preset x64
ctest --preset x64
```

Without `AXIAL_OPENSSL_ROOT`, CMake downloads and builds the pinned OpenSSL
itself. Boost 1.90.0 is fetched by CMake and checked against its hash.

| Preset | Purpose |
| --- | --- |
| `x64` | Visual Studio 2022, x64: everything plus tests (`build/x64`) |
| `x86-adapters` | 32-bit `siappdll.dll` and `TDxNavLib.dll` only, for 32-bit clients (`build/x86`) |
| `mingw` | Cross-compile from Linux with mingw-w64; tests run under Wine |
| `catalog` | Device catalog only, for running the C# model tests on Linux or macOS |

CTest covers raw HID replay, sparse device buttons, malformed reports, key
release, IPC, ownership of the app-owned service, the siappdll and Navlib
adapters through real windows and message loops, the web server and certificate
setup, configuration and logging. The integration tests start a private mock
service with temporary sockets and settings; they never inject input into other
applications or touch the installed configuration.

### Cross build under Wine

The `mingw` preset is useful for compile checks on Linux. Wine has no `AF_UNIX`
sockets and does not keep protected DACLs, so the transport, owner and
integration tests only pass on Windows; core, application and web tests pass
under Wine.

```sh
cmake --preset mingw -DAXIAL_OPENSSL_ROOT=/path/to/openssl-mingw
cmake --build --preset mingw
ctest --preset mingw -R "core|application|web"
```

## Settings app

```powershell
dotnet test app/Axial.Core.Tests -p:AxialBridgeDirectory=$PWD\build\x64\bin\Release
dotnet publish app/Axial/Axial.csproj -c Release -r win-x64 --self-contained -o out/app
```

`Axial.Core` holds all behaviour that axial's Swift `Model` had, so it is tested
without a UI. On Linux, build the `catalog` preset and pass
`-p:AxialBridgeDirectory=$PWD/build/catalog` to run the same tests.
To try the app from a build tree, copy `axial-service.exe`, `axial-bridge.dll`,
`axial-web-setup.exe` and `axialctl.exe` next to `Axial.exe`.

For UI changes, check the window at its minimum size with long device names,
two-button and sparse-button devices, light/dark and high-contrast modes, and a
full Test event log.

## Installer

```powershell
cmake --preset x86-adapters
cmake --build --preset x86-adapters
iscc /DAppVersion=0.2.4 /DSourceRoot=$PWD installer\Axial.iss
```

The installer is written to `out/installer`. It installs the app and service to
`Program Files\Axial`, the compatibility DLLs to `System32` and `SysWOW64`, and
removes the web certificate and login item on uninstall. CI builds the same
installer and uploads it as the `Axial-Windows` artifact.

## Porting notes

The Windows code follows axial's sources closely so fixes can be carried across:

| macOS | Windows |
| --- | --- |
| IOKit HID manager | SetupDi enumeration, `RegisterDeviceNotification`, `HidD_*`/`HidP_*`, overlapped `ReadFile` |
| CGEvent keyboard shortcuts | `SendInput` |
| `NSWorkspace` frontmost app | `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`; app id is the lowercased executable name |
| Unix sockets in `/tmp/axial-$UID` | `AF_UNIX` sockets in `%LOCALAPPDATA%\Axial`, peer checked with `SIO_AF_UNIX_GETPEERPID` and the process token's SID |
| Dispatch queues, `CVDisplayLink` | Message-only windows per session, thread pool, `DwmFlush` frame clock |
| `3DconnexionClient.framework` | `siappdll.dll` (`SpaceWareMessage00` window messages) |
| `3DconnexionNavlib.framework` | `TDxNavLib.dll` |
| Keychain and admin trust settings | Key file with a protected DACL; CA in the CurrentUser root store |
| Loopback alias for 127.51.68.120 | Not needed: all of 127/8 is loopback on Windows |
