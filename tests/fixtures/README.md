# Test fixtures

- `spaceexplorer-motion.txt`: 4,380 real SpaceExplorer USB motion reports,
  covering both signs on all six axes and return to neutral. The native core
  test reads this file. Its columns are documented in the file header.
- `upstream-device-buttons.toml`: unmodified PySpaceMouse device definitions.
  `device-buttons-UPSTREAM` pins the revision and `device-buttons-LICENSE`
  preserves the MIT notice. `tests/device_cases.hpp` reads selected layouts.
- `spacemouse-LICENSE`: MIT notice for raw-HID cases adapted in
  `tests/core_tests.cpp` from [nytamin/spacemouse](https://github.com/nytamin/spacemouse/blob/787929591f8cc282c8823681934e6f7f8a401ae7/packages/core/src/__tests__/incoming-data.spec.ts).

Fixtures are test inputs, not generated build output. They are not installed
with Axial.
