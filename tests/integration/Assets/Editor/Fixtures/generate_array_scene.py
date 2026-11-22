"""Generate the original, self-contained array_scene.gltf regression fixture."""
import base64
import json
from pathlib import Path
import struct
import zlib


def uri(data, mime):
    return f"data:{mime};base64," + base64.b64encode(data).decode("ascii")


def png(kind, size):
    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))

    pixels = bytearray()
    for row in range(size):
        y = row * 16 // size
        pixels.append(0)
        for column in range(size):
            x = column * 16 // size
            if kind < 2:
                step = 16 * ((x // 4 + y // 4) % 2)
                value = (180 + step, 50, 40, 255) if kind == 0 else (40, 80, 180 + step, 255)
            elif kind < 4:
                value = (120 + x if kind == 2 else 136 - x, 120 + y, 254, 255)
            elif kind < 6:
                shade = 170 + (x if kind == 4 else y) * 5
                value = (shade, shade, shade, 255)
            else:
                value = (60, 220, 70, 255 if (x // 4 + y // 4) % 2 else 0)
            pixels.extend(value)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(pixels, 9)) + chunk(b"IEND", b""))


def generate(size=16, filename="array_scene.gltf", alpha_mask=True):
    buffer = bytearray()
    views, accessors = [], []

    def accessor(values, components, bounds=None, indices=False):
        while len(buffer) % 4:
            buffer.append(0)
        data = struct.pack("<" + ("H" if indices else "f") * len(values), *values)
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)})
        buffer.extend(data)
        result = {"bufferView": len(views) - 1, "componentType": 5123 if indices else 5126,
                  "count": len(values) // components, "type": {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}[components]}
        if bounds:
            result.update(min=bounds[0], max=bounds[1])
        accessors.append(result)
        return len(accessors) - 1

    position = accessor([-.45, -.45, 0, .45, -.45, 0, .45, .45, 0, -.45, .45, 0], 3,
                        ([-.45, -.45, 0], [.45, .45, 0]))
    normal = accessor([0, 0, 1] * 4, 3)
    tangent = accessor([1, 0, 0, 1] * 4, 4)
    uv = accessor([0, 0, 1, 0, 1, 1, 0, 1], 2)
    indices = accessor([0, 1, 2, 0, 2, 3], 1, indices=True)
    materials = []
    for layer in range(2):
        transform = {"offset": [.1, .1], "scale": [.8, .8]}
        if layer:
            transform = {"offset": [.2, .15], "scale": [.6, .7]}
        materials.append({"name": f"layer_{layer}", "doubleSided": True,
                          "pbrMetallicRoughness": {"metallicFactor": 0, "roughnessFactor": .5,
                              "baseColorTexture": {"index": layer, "extensions": {"KHR_texture_transform": transform}}},
                          "normalTexture": {"index": layer + 2}, "occlusionTexture": {"index": layer + 4}})
    materials.append({"name": "repeating_mask_standalone", "doubleSided": True, "alphaMode": "MASK",
                      "alphaCutoff": .5, "pbrMetallicRoughness": {"metallicFactor": 0, "roughnessFactor": .8,
                          "baseColorTexture": {"index": 6, "extensions": {"KHR_texture_transform": {"scale": [3, 2]}}}}})
    if not alpha_mask:
        # Upload measurements require every image to have identical offline mips.
        # Alpha-mask coverage intentionally rejects the full chain in the render fixture.
        materials[-1]["name"] = "repeating_opaque_standalone"
        materials[-1]["alphaMode"] = "OPAQUE"
        del materials[-1]["alphaCutoff"]
    meshes = []
    for material in range(3):
        primitive = {"attributes": {"POSITION": position, "NORMAL": normal, "TANGENT": tangent, "TEXCOORD_0": uv},
                     "indices": indices, "material": material}
        if material < 2:
            primitive["extensions"] = {"KHR_materials_variants": {"mappings": [{"material": 1 - material, "variants": [0]}]}}
        meshes.append({"name": f"panel_{material}", "primitives": [primitive]})
    nodes = [{"name": "red", "mesh": 0, "translation": [-1.1, -.55, 0]},
             {"name": "blue", "mesh": 1, "translation": [0, -.55, 0]},
             {"name": "mask", "mesh": 2, "translation": [1.1, -.55, 0]},
             {"name": "shared_red", "mesh": 0, "translation": [-.55, .55, 0], "scale": [.8, .8, .8]},
             {"name": "shared_blue", "mesh": 1, "translation": [.55, .55, 0], "scale": [.8, .8, .8]}]
    scene = {"asset": {"version": "2.0", "generator": "ZenEngine generate_array_scene.py (original test fixture)"},
             "extensionsUsed": ["KHR_texture_transform", "KHR_materials_variants"],
             "extensions": {"KHR_materials_variants": {"variants": [{"name": "swap_layers"}]}},
             "scene": 0, "scenes": [{"nodes": list(range(len(nodes)))}], "nodes": nodes, "meshes": meshes,
             "buffers": [{"uri": uri(buffer, "application/octet-stream"), "byteLength": len(buffer)}],
             "bufferViews": views, "accessors": accessors, "materials": materials,
             "images": [{"name": f"image_{i}", "uri": uri(png(i, size), "image/png")} for i in range(7)],
             "textures": [{"source": i, "sampler": 1 if i == 6 else 0} for i in range(7)],
             "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071},
                          {"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]}
    output = Path(filename)
    if not output.is_absolute():
        output = Path(__file__).with_name(filename)
    output.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    generate()
    generate(128, "array_scene_minification.gltf")
