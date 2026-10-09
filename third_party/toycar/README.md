# Toy Car

Upstream: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ToyCar
Pinned revision: see REVISION. Original downloaded files are in source/.

Mesh: Guido Odendahl (2020). Materials and scene: Eric Chadwick (2020).
License: CC0-1.0, included in CC0-1.0.txt and upstream README.

The settings app's Test tab loads source/ToyCar.gltf at runtime with a small
glTF reader (`app/Axial/Scene/GltfLoader.cs`). Like axial, it omits the fabric
display stand and upstream cameras, centers the car and scales its longest
dimension to 2.8 scene units. Only base colour is used; the app supplies its own
lights. The reader intentionally handles only this pinned, trusted asset.
