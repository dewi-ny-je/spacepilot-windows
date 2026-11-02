# Upstream sync

This port follows [consi/axial](https://github.com/consi/axial) (`main`). A
weekly check reviews new upstream commits and ports the ones that apply to
Windows; macOS-only changes (Swift app, Xcode, Homebrew, signing) are skipped.

Last reviewed upstream commit: `b6b391a72878464834b47bfd9bd46b42922887df`

| Reviewed | Upstream range | Ported |
| --- | --- | --- |
| 2026-10-29 | up to `0464842` (v0.2.4, 2026-09-25) | Already in the initial port |
| 2026-11-02 | `18fb3ae`..`b6b391a` (v0.2.5) | Web API fixes for clients that ignore `self:update` and send undeclared row-major matrices (`18fb3ae`, `1d86f6c`, `08a1279`, `8ab6b0b`); hardware calibrate (`b6b391a`) was already ported, only its extra invalid-device test cases added |
