using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Media.Media3D;

namespace Axial.App;

// A small glTF 2.0 reader for the bundled, trusted test model: triangle meshes
// with positions, normals, UVs, base colour (factor and texture) and emission.
// Node transforms are baked into the vertices, the result is centred and its
// longest side scaled to `size`, and every resource is frozen so the model can
// be built on a worker thread.
static class GltfLoader {
    sealed record Primitive(Point3D[] Positions, Vector3D[] Normals, Point[]? Uvs, int[] Indices, Material Front, Material? Back);

    public static Model3DGroup Load(string path, Func<string, bool>? skipNode = null, double size = 2.8) {
        string directory = Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".";
        using var document = JsonDocument.Parse(File.ReadAllBytes(path));
        var root = document.RootElement;
        byte[][] buffers = Array(root, "buffers").Select(b => ReadUri(directory, b.GetProperty("uri").GetString() ?? "")).ToArray();
        JsonElement[] views = Array(root, "bufferViews").ToArray();
        JsonElement[] accessors = Array(root, "accessors").ToArray();
        JsonElement[] meshes = Array(root, "meshes").ToArray();
        JsonElement[] nodes = Array(root, "nodes").ToArray();
        Dictionary<int, ImageSource?> images = [];
        JsonElement[] materials = Array(root, "materials").ToArray();
        JsonElement[] textures = Array(root, "textures").ToArray();
        JsonElement[] imageDefinitions = Array(root, "images").ToArray();

        (byte[] Data, int Offset, int Stride, int Count, int Type) Access(int index, int components) {
            var accessor = accessors[index];
            var view = views[accessor.GetProperty("bufferView").GetInt32()];
            int type = accessor.GetProperty("componentType").GetInt32();
            int width = type switch { 5126 or 5125 => 4, 5123 or 5122 => 2, _ => 1 };
            int offset = Int(view, "byteOffset") + Int(accessor, "byteOffset");
            int stride = view.TryGetProperty("byteStride", out var s) ? s.GetInt32() : width * components;
            int count = accessor.GetProperty("count").GetInt32();
            var data = buffers[view.GetProperty("buffer").GetInt32()];
            if (count <= 0 || offset < 0 || offset + (long)(count - 1) * stride + width * components > data.Length) throw new InvalidDataException("A glTF accessor exceeds its buffer.");
            return (data, offset, stride, count, type);
        }
        float[] Floats(int index, int components) {
            var (data, offset, stride, count, type) = Access(index, components);
            if (type != 5126) throw new InvalidDataException("Only float vertex attributes are supported.");
            var result = new float[count * components];
            for (int i = 0; i < count; i++)
                for (int c = 0; c < components; c++) result[i * components + c] = BitConverter.ToSingle(data, offset + i * stride + c * 4);
            return result;
        }
        int[] Indices(int index) {
            var (data, offset, _, count, type) = Access(index, 1);
            int width = type switch { 5125 => 4, 5123 => 2, _ => 1 };
            var result = new int[count];
            for (int i = 0; i < count; i++) {
                int at = offset + i * width;
                result[i] = type switch { 5125 => (int)BitConverter.ToUInt32(data, at), 5123 => BitConverter.ToUInt16(data, at), 5121 => data[at], _ => throw new InvalidDataException("Unsupported index type.") };
            }
            return result;
        }
        ImageSource? Texture(JsonElement? reference) {
            if (reference is not { } r || !r.TryGetProperty("index", out var t)) return null;
            if (!textures[t.GetInt32()].TryGetProperty("source", out var s)) return null;
            int source = s.GetInt32();
            if (images.TryGetValue(source, out var cached)) return cached;
            ImageSource? image = null;
            if (imageDefinitions[source].TryGetProperty("uri", out var uri)) {
                var bitmap = new BitmapImage();
                bitmap.BeginInit();
                bitmap.CacheOption = BitmapCacheOption.OnLoad;
                bitmap.StreamSource = new MemoryStream(ReadUri(directory, uri.GetString() ?? ""));
                bitmap.EndInit(); bitmap.Freeze();
                image = bitmap;
            }
            return images[source] = image;
        }
        Brush Paint(ImageSource image, Color tint) {
            // Texture coordinates are absolute (0…1), not relative to the mesh's UV bounds.
            var brush = new ImageBrush(image) { ViewportUnits = BrushMappingMode.Absolute, Viewport = new Rect(0, 0, 1, 1), TileMode = TileMode.Tile, Opacity = tint.A / 255.0 };
            brush.Freeze();
            return brush;
        }
        Dictionary<int, (Material, Material?)> cache = [];
        (Material, Material?) MaterialFor(int index) {
            if (cache.TryGetValue(index, out var made)) return made;
            var definition = index >= 0 && index < materials.Length ? materials[index] : default;
            var group = new MaterialGroup();
            Color baseColor = Colors.White;
            JsonElement? baseTexture = null;
            bool transmission = false, doubleSided = false;
            if (definition.ValueKind == JsonValueKind.Object) {
                if (definition.TryGetProperty("pbrMetallicRoughness", out var pbr)) {
                    if (pbr.TryGetProperty("baseColorFactor", out var factor)) baseColor = ColorOf(factor);
                    if (pbr.TryGetProperty("baseColorTexture", out var texture)) baseTexture = texture;
                }
                transmission = definition.TryGetProperty("extensions", out var extensions) && extensions.TryGetProperty("KHR_materials_transmission", out _);
                doubleSided = definition.TryGetProperty("doubleSided", out var sided) && sided.ValueKind == JsonValueKind.True;
            }
            if (transmission) {
                // No transmission in WPF: approximate glass with tinted transparency, as axial does.
                group.Children.Add(new DiffuseMaterial(new SolidColorBrush(Color.FromArgb(64, 89, 153, 115))));
                group.Children.Add(new SpecularMaterial(new SolidColorBrush(Color.FromArgb(150, 255, 255, 255)), 60));
                doubleSided = true;
            } else {
                var image = Texture(baseTexture);
                group.Children.Add(new DiffuseMaterial(image != null ? Paint(image, baseColor) : new SolidColorBrush(baseColor)));
                group.Children.Add(new SpecularMaterial(new SolidColorBrush(Color.FromArgb(70, 255, 255, 255)), 35));
                if (definition.ValueKind == JsonValueKind.Object && definition.TryGetProperty("emissiveTexture", out var emissive) && Texture(emissive) is { } glow)
                    group.Children.Add(new EmissiveMaterial(Paint(glow, Colors.White)));
            }
            group.Freeze();
            return cache[index] = (group, doubleSided ? group : null);
        }

        List<Primitive> primitives = [];
        void Visit(int index, Matrix3D parent) {
            var node = nodes[index];
            if (node.TryGetProperty("name", out var name) && skipNode?.Invoke(name.GetString() ?? "") == true) return;
            var world = Local(node) * parent;
            if (node.TryGetProperty("mesh", out var meshIndex)) {
                foreach (var primitive in Array(meshes[meshIndex.GetInt32()], "primitives")) {
                    if (Int(primitive, "mode", 4) != 4 || !primitive.TryGetProperty("indices", out var indexAccessor)) continue;
                    var attributes = primitive.GetProperty("attributes");
                    var positions = Floats(attributes.GetProperty("POSITION").GetInt32(), 3);
                    int count = positions.Length / 3;
                    var points = new Point3D[count];
                    for (int i = 0; i < count; i++) points[i] = world.Transform(new Point3D(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]));
                    var normals = new Vector3D[attributes.TryGetProperty("NORMAL", out var n) ? count : 0];
                    if (normals.Length > 0) {
                        var values = Floats(n.GetInt32(), 3);
                        for (int i = 0; i < count; i++) { var v = world.Transform(new Vector3D(values[i * 3], values[i * 3 + 1], values[i * 3 + 2])); v.Normalize(); normals[i] = v; }
                    }
                    Point[]? uvs = null;
                    if (attributes.TryGetProperty("TEXCOORD_0", out var t)) {
                        var values = Floats(t.GetInt32(), 2);
                        uvs = new Point[count];
                        for (int i = 0; i < count; i++) uvs[i] = new Point(values[i * 2], values[i * 2 + 1]);
                    }
                    var indices = Indices(indexAccessor.GetInt32());
                    if (indices.Any(i => i < 0 || i >= count)) throw new InvalidDataException("A glTF index is out of range.");
                    var (front, back) = MaterialFor(Int(primitive, "material", -1));
                    primitives.Add(new Primitive(points, normals, uvs, indices, front, back));
                }
            }
            if (node.TryGetProperty("children", out var children))
                foreach (var child in children.EnumerateArray()) Visit(child.GetInt32(), world);
        }
        int scene = Int(root, "scene");
        var roots = root.TryGetProperty("scenes", out var scenes) ? scenes[scene].GetProperty("nodes").EnumerateArray().Select(n => n.GetInt32()) : Enumerable.Range(0, nodes.Length);
        foreach (var index in roots) Visit(index, Matrix3D.Identity);
        if (primitives.Count == 0) throw new InvalidDataException("The model has no triangles.");

        var bounds = Rect3D.Empty;
        foreach (var primitive in primitives) foreach (var p in primitive.Positions) bounds.Union(p);
        double scale = size / Math.Max(bounds.SizeX, Math.Max(bounds.SizeY, bounds.SizeZ));
        var center = new Point3D(bounds.X + bounds.SizeX / 2, bounds.Y + bounds.SizeY / 2, bounds.Z + bounds.SizeZ / 2);
        var result = new Model3DGroup();
        foreach (var primitive in primitives) {
            var mesh = new MeshGeometry3D {
                Positions = new Point3DCollection(primitive.Positions.Select(p => (Point3D)((p - center) * scale))),
                TriangleIndices = new Int32Collection(primitive.Indices),
            };
            if (primitive.Normals.Length > 0) mesh.Normals = new Vector3DCollection(primitive.Normals);
            if (primitive.Uvs != null) mesh.TextureCoordinates = new PointCollection(primitive.Uvs);
            mesh.Freeze();
            result.Children.Add(new GeometryModel3D(mesh, primitive.Front) { BackMaterial = primitive.Back });
        }
        result.Freeze();
        return result;
    }

    static IEnumerable<JsonElement> Array(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Array ? value.EnumerateArray() : Enumerable.Empty<JsonElement>();
    static int Int(JsonElement element, string name, int fallback = 0) => element.TryGetProperty(name, out var value) ? value.GetInt32() : fallback;
    static double[] Numbers(JsonElement element) => element.EnumerateArray().Select(v => v.GetDouble()).ToArray();
    static Color ColorOf(JsonElement factor) {
        var c = Numbers(factor);
        static byte Byte(double v) => (byte)Math.Round(Math.Clamp(v, 0, 1) * 255);
        return Color.FromArgb(Byte(c.Length > 3 ? c[3] : 1), Byte(c[0]), Byte(c[1]), Byte(c[2]));
    }
    static byte[] ReadUri(string directory, string uri) {
        const string marker = ";base64,";
        if (uri.StartsWith("data:", StringComparison.Ordinal)) return Convert.FromBase64String(uri[(uri.IndexOf(marker, StringComparison.Ordinal) + marker.Length)..]);
        var path = Path.GetFullPath(Path.Combine(directory, Uri.UnescapeDataString(uri)));
        if (!path.StartsWith(directory, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("A glTF resource is outside the model folder.");
        return File.ReadAllBytes(path);
    }
    // glTF matrices are column-major for column vectors; WPF uses row vectors,
    // so the same 16 numbers fill a Matrix3D row by row.
    static Matrix3D Local(JsonElement node) {
        if (node.TryGetProperty("matrix", out var m)) {
            var v = Numbers(m);
            return new Matrix3D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
        }
        var matrix = Matrix3D.Identity;
        if (node.TryGetProperty("scale", out var s)) { var v = Numbers(s); matrix.Scale(new Vector3D(v[0], v[1], v[2])); }
        if (node.TryGetProperty("rotation", out var r)) { var v = Numbers(r); matrix.Rotate(new Quaternion(v[0], v[1], v[2], v[3])); }
        if (node.TryGetProperty("translation", out var t)) { var v = Numbers(t); matrix.Translate(new Vector3D(v[0], v[1], v[2])); }
        return matrix;
    }
}
