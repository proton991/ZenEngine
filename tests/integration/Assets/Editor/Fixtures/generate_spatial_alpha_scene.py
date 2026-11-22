"""Generate an original alpha-mask/blend scene for cooked spatial ray comparisons."""
import base64
import copy
import json
from pathlib import Path
import struct
import zlib
from generate_array_scene import generate, uri


def mask_png(index):
    width, height = ((16, 8) if index % 2 == 0 else (8, 16))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    data = bytearray()
    for y in range(height):
        data.append(0)
        for x in range(width):
            alpha = 255 if ((x // 2 + y // 2 + index) % 2) else 0
            data.extend((50 + index * 40, 210 - index * 30, 80, alpha))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b""))


def generate_alpha():
    path = Path(__file__).with_name("spatial_alpha_scene.gltf")
    generate(16, path)
    scene = json.loads(path.read_text(encoding="utf-8"))
    payload = bytearray(base64.b64decode(scene["buffers"][0]["uri"].split(",")[1]))
    def accessor(values, components):
        while len(payload) % 4:
            payload.append(0)
        data = struct.pack("<" + "f" * len(values), *values)
        scene["bufferViews"].append({"buffer": 0, "byteOffset": len(payload), "byteLength": len(data)})
        payload.extend(data)
        scene["accessors"].append({"bufferView": len(scene["bufferViews"]) - 1, "componentType": 5126,
                                   "count": len(values) // components, "type": f"VEC{components}"})
        return len(scene["accessors"]) - 1
    uv1 = accessor([1, 0, 0, 0, 0, 1, 1, 1], 2)
    colors = accessor([1, 1, 1, .8, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, .8], 4)
    materials = []
    for index in range(6):
        texture = index if index < 4 else (0 if index == 4 else 2)
        transform = {"offset": [.125, .125], "scale": [.75, .75]}
        binding = {"index": texture, "texCoord": 1 if index in (1, 3, 5) else 0,
                   "extensions": {"KHR_texture_transform": transform}}
        material = {"name": f"alpha_{index}", "doubleSided": True,
                    "alphaMode": "MASK" if index < 4 else "BLEND", "alphaCutoff": .5}
        if index in (2, 3, 5):
            material["extensions"] = {"KHR_materials_pbrSpecularGlossiness": {
                "diffuseFactor": [1, 1, 1, .9], "diffuseTexture": binding,
                "specularFactor": [0, 0, 0], "glossinessFactor": .5}}
        else:
            material["pbrMetallicRoughness"] = {"baseColorFactor": [1, 1, 1, .9], "baseColorTexture": binding,
                                                "metallicFactor": 0, "roughnessFactor": .8}
        materials.append(material)
    materials.append({"name": "opaque_rear", "pbrMetallicRoughness": {"baseColorFactor": [.3,.3,.3,1]}})
    primitive = copy.deepcopy(scene["meshes"][0]["primitives"][0])
    primitive["attributes"]["TEXCOORD_1"] = uv1
    primitive["attributes"]["COLOR_0"] = colors
    meshes = []
    for index in range(7):
        value = copy.deepcopy(primitive)
        value["material"] = index
        value.pop("extensions", None)
        if index < 6:
            alternate = index ^ 1
            value["extensions"] = {"KHR_materials_variants": {"mappings": [{"material": alternate, "variants": [0]}]}}
        meshes.append({"name": f"alpha_panel_{index}", "primitives": [value]})
    nodes = []
    for rear in (False, True):
        for index in range(6):
            nodes.append({"name": ("rear_" if rear else "front_") + str(index), "mesh": 6 if rear else index,
                          "translation": [(index % 3 - 1)*1.25, (index//3-.5)*1.25, .25 if rear else 0]})
    scene.update(materials=materials, meshes=meshes, nodes=nodes, scenes=[{"nodes": list(range(12))}],
                 images=[{"name": f"mask_{i}", "uri": uri(mask_png(i), "image/png")} for i in range(4)],
                 textures=[{"source": i, "sampler": 0} for i in range(4)],
                 samplers=[{"magFilter":9729,"minFilter":9729,"wrapS":33071,"wrapT":33071}])
    scene["asset"]["generator"] = "ZenEngine generate_spatial_alpha_scene.py (original fixture)"
    scene["extensionsUsed"].append("KHR_materials_pbrSpecularGlossiness")
    scene["buffers"][0] = {"uri": uri(payload,"application/octet-stream"), "byteLength": len(payload)}
    path.write_text(json.dumps(scene,indent=2)+"\n",encoding="utf-8")


if __name__ == "__main__":
    generate_alpha()
