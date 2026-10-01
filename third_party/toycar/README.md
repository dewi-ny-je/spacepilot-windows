# Toy Car

Upstream: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ToyCar
Pinned revision: see REVISION. Original downloaded files are in source/.

Mesh: Guido Odendahl (2020). Materials and scene: Eric Chadwick (2020).
License: CC0-1.0, included in CC0-1.0.txt and upstream README.

Axial omits the fabric display stand and upstream cameras, centers the car
and scales its longest dimension to 2.8 scene units. Base color, normal, emission,
occlusion, roughness, metalness and clear-coat textures are embedded in the native
SceneKit archive. Glass transmission is approximated with tinted transparency.
The app supplies its own studio lights and reflection environment.

CMake builds the native converter in `tools/convert-toycar.swift` and generates
`build/<preset>/generated/ToyCar.scn`. Source checksums are verified before
conversion. Regenerate from the repository root:

```sh
cmake --preset native
cmake --build --preset native --target axial-assets
```

The converter intentionally handles only this pinned, trusted asset. It is not a
runtime importer. The installed app loads the self-contained SceneKit archive.
