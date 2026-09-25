"""GPU smoke and image regressions for scene_renderer_demo (standard-library only).

Run from any directory after building the demo: python tools/validate_voxel_gi.py
Temporarily overrides Data/engine.cfg and restores its exact bytes in finally.
Generated fixtures, logs, and PPM/PNG captures stay under build/voxel-gi-validation.
"""

import copy
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/voxel-gi-validation"


def make_fixture():
    data = bytearray()
    views, accessors, primitives = [], [], []

    def attribute(values, component, kind):
        start = len(data)
        flat = [v for item in values for v in (item if isinstance(item, tuple) else (item,))]
        data.extend(struct.pack("<" + ("f" if component == 5126 else "I") * len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=start, byteLength=len(data) - start))
        accessors.append(dict(bufferView=len(views) - 1, componentType=component,
                              count=len(values), type=kind))
        return len(accessors) - 1

    def quad(vertices, normal, material):
        position = attribute(vertices, 5126, "VEC3")
        accessors[position].update(min=[min(v[i] for v in vertices) for i in range(3)],
                                   max=[max(v[i] for v in vertices) for i in range(3)])
        normals = attribute([normal] * 4, 5126, "VEC3")
        indices = attribute([0, 1, 2, 0, 2, 3], 5125, "SCALAR")
        primitives.append(dict(attributes={"POSITION": position, "NORMAL": normals},
                               indices=indices, material=material))

    quad([(-1, -1, 1), (1, -1, 1), (1, -1, -1), (-1, -1, -1)], (0, 1, 0), 0)
    quad([(-1, 1, -1), (1, 1, -1), (1, 1, 1), (-1, 1, 1)], (0, -1, 0), 0)
    quad([(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1)], (0, 0, 1), 0)
    quad([(-1, -1, 1), (-1, -1, -1), (-1, 1, -1), (-1, 1, 1)], (1, 0, 0), 1)
    quad([(1, -1, -1), (1, -1, 1), (1, 1, 1), (1, 1, -1)], (-1, 0, 0), 2)
    quad([(-.45, .85, -.35), (.45, .85, -.35), (.45, .85, .35), (-.45, .85, .35)],
         (0, -1, 0), 3)
    materials = [dict(pbrMetallicRoughness=dict(baseColorFactor=[*rgb, 1],
                                             metallicFactor=0, roughnessFactor=1))
                 for rgb in ([.8, .8, .8], [.8, .025, .015], [.015, .65, .025], [1, 1, 1])]
    for variant in ("room", "emissive"):
        materials[3]["emissiveFactor"] = [1, .625, .125] if variant == "emissive" else [0, 0, 0]
        materials[3]["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": 8}}
        document = dict(asset={"version": "2.0"}, extensionsUsed=["KHR_materials_emissive_strength"],
                        buffers=[dict(uri="room.bin", byteLength=len(data))], bufferViews=views,
                        accessors=accessors, materials=materials, meshes=[dict(primitives=primitives)],
                        nodes=[dict(mesh=0)], scenes=[dict(nodes=[0])], scene=0)
        (OUT / f"{variant}.gltf").write_text(json.dumps(document))
    (OUT / "room.bin").write_bytes(data)

    gray_colors = attribute([(.5, .5, .5)] * 4, 5126, "VEC3")
    rgba_colors = attribute([(.5, .5, .5, 1)] * 4, 5126, "VEC4")
    transparent_colors = attribute([(1, 1, 1, 0)] * 4, 5126, "VEC4")
    color_accessors = {"vertex": gray_colors, "vertex-rgba": rgba_colors,
                       "transparent-rgba": transparent_colors}
    for variant in ("factor", "vertex", "masked", "opaque", "vertex-rgba",
                    "transparent", "transparent-rgba"):
        material_scene = copy.deepcopy(document)
        material_scene["buffers"] = [dict(uri="material.bin", byteLength=len(data))]
        for material in material_scene["materials"]:
            rgb = [.5] * 3 if variant == "factor" else [1] * 3
            alpha = 0 if variant == "transparent" else (.7 if variant == "masked" else 1)
            material["pbrMetallicRoughness"]["baseColorFactor"] = [*rgb, alpha]
            material["emissiveFactor"] = [0, 0, 0]
            material.update(alphaMode="MASK", alphaCutoff=.5)
        if variant in color_accessors:
            for primitive in material_scene["meshes"][0]["primitives"]:
                primitive["attributes"]["COLOR_0"] = color_accessors[variant]
        (OUT / f"material-{variant}.gltf").write_text(json.dumps(material_scene))
    (OUT / "material.bin").write_bytes(data)


def make_surface_contract_fixtures():
    """Equivalent encodings plus finite shadow segments; used by image assertions below."""
    room = json.loads((OUT / "room.gltf").read_text(encoding="utf-8"))
    binary = (OUT / "room.bin").read_bytes()

    def save(name, document):
        (OUT / f"{name}.gltf").write_text(json.dumps(document), encoding="utf-8")

    padded = copy.deepcopy(room)
    data = bytearray()
    for index, view in enumerate(padded["bufferViews"]):
        source = room["bufferViews"][index]
        content = binary[source["byteOffset"]:source["byteOffset"] + source["byteLength"]]
        view["byteOffset"] = len(data)
        accessor = next(a for a in padded["accessors"] if a["bufferView"] == index)
        if accessor["type"] == "VEC3":
            view["byteStride"] = 16
            for offset in range(0, len(content), 12):
                data.extend(content[offset:offset + 12] + bytes(4))
        else:
            data.extend(content)
        view["byteLength"] = len(data) - view["byteOffset"]
    padded["buffers"] = [dict(uri="surface-padded.bin", byteLength=len(data))]
    (OUT / "surface-padded.bin").write_bytes(data)
    save("surface-padded", padded)
    no_normals = copy.deepcopy(room)
    for primitive in no_normals["meshes"][0]["primitives"]:
        del primitive["attributes"]["NORMAL"]
    save("surface-no-normals", no_normals)

    explicit = copy.deepcopy(room)
    explicit["materials"] = [dict(emissiveFactor=[1, 0, 0]), {}]
    for primitive in explicit["meshes"][0]["primitives"]:
        primitive["material"] = 1
    save("surface-default-explicit", explicit)
    omitted = copy.deepcopy(explicit)
    for primitive in omitted["meshes"][0]["primitives"]:
        del primitive["material"]
    save("surface-default-omitted", omitted)
    del omitted["materials"]
    save("surface-default-only", omitted)

    # A gray image makes sRGB/linear confusion observable. All three assets sample
    # identical bytes: distinct images, shared image, and one texture used in both roles.
    textured = copy.deepcopy(room)
    uv_bytes = struct.pack("<8f", 0, 0, 1, 0, 1, 1, 0, 1)
    textured["bufferViews"].append(dict(buffer=0, byteOffset=len(binary), byteLength=len(uv_bytes)))
    textured["accessors"].append(dict(bufferView=len(textured["bufferViews"]) - 1,
                                       componentType=5126, count=4, type="VEC2"))
    textured["buffers"] = [dict(uri="surface-textured.bin", byteLength=len(binary) + len(uv_bytes))]
    (OUT / "surface-textured.bin").write_bytes(binary + uv_bytes)
    textured["images"] = [dict(uri='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGNoaGj4D8IMMAYAVvQJ/UtL6SwAAAAASUVORK5CYII='), dict(uri='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGNoaGj4D8IMMAYAVvQJ/UtL6SwAAAAASUVORK5CYII=')]
    textured["textures"] = [dict(source=0), dict(source=1)]
    textured["materials"] = [dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=0),
                                metallicRoughnessTexture=dict(index=1), metallicFactor=1, roughnessFactor=1))]
    for primitive in textured["meshes"][0]["primitives"]:
        primitive["material"] = 0
        primitive["attributes"]["TEXCOORD_0"] = len(textured["accessors"]) - 1
    save("surface-texture-split", textured)
    textured["textures"][1]["source"] = 0
    textured["images"] = textured["images"][:1]
    save("surface-texture-shared-image", textured)
    textured["textures"] = textured["textures"][:1]
    textured["materials"][0]["pbrMetallicRoughness"]["metallicRoughnessTexture"]["index"] = 0
    save("surface-texture-shared-slot", textured)

    # Normalized receiver z=-.5 and point/spot light z=-.455. The blocker is either
    # behind the light (-.44) or between the biased origin and the light (-.46).
    for name, depth in (("behind", -.88), ("before", -.92)):
        shadow = copy.deepcopy(room)
        data = bytearray(binary)
        attributes = {}
        for semantic, values in (("POSITION", [(-.6, -.6, depth), (-.6, .6, depth),
                                               (.6, .6, depth), (.6, -.6, depth)]),
                                 ("NORMAL", [(0, 0, -1)] * 4)):
            start = len(data)
            data.extend(struct.pack("<12f", *(value for row in values for value in row)))
            shadow["bufferViews"].append(dict(buffer=0, byteOffset=start, byteLength=48))
            accessor = dict(bufferView=len(shadow["bufferViews"]) - 1,
                            componentType=5126, count=4, type="VEC3")
            if semantic == "POSITION":
                accessor.update(min=[-.6, -.6, depth], max=[.6, .6, depth])
            shadow["accessors"].append(accessor)
            attributes[semantic] = len(shadow["accessors"]) - 1
        shadow["meshes"][0]["primitives"].append(dict(attributes=attributes, indices=2, material=0))
        shadow["buffers"] = [dict(uri=f"shadow-{name}.bin", byteLength=len(data))]
        (OUT / f"shadow-{name}.bin").write_bytes(data)
        save(f"shadow-{name}", shadow)


def make_mesh_shadow_fixtures():
    """A back-facing slanted triangle casts onto the visible room wall, without hiding it."""
    document = json.loads((OUT / "room.gltf").read_text(encoding="utf-8"))
    data = bytearray((OUT / "room.bin").read_bytes())
    attributes = {}
    for semantic, values in (("POSITION", [(-.24, -.24, -.3), (-.24, .24, -.3), (.24, -.24, -.3)]),
                             ("NORMAL", [(0, 0, -1)] * 3)):
        start = len(data)
        data.extend(struct.pack("<9f", *(component for row in values for component in row)))
        document["bufferViews"].append(dict(buffer=0, byteOffset=start, byteLength=36))
        accessor = dict(bufferView=len(document["bufferViews"]) - 1, componentType=5126,
                        count=3, type="VEC3")
        if semantic == "POSITION":
            accessor.update(min=[-.24, -.24, -.3], max=[.24, .24, -.3])
        document["accessors"].append(accessor)
        attributes[semantic] = len(document["accessors"]) - 1
    start = len(data)
    data.extend(struct.pack("<3I", 0, 1, 2))
    document["bufferViews"].append(dict(buffer=0, byteOffset=start, byteLength=12))
    document["accessors"].append(dict(bufferView=len(document["bufferViews"]) - 1,
                                      componentType=5125, count=3, type="SCALAR"))
    material_index = len(document["materials"])
    document["materials"].append(dict(pbrMetallicRoughness=dict(baseColorFactor=[1, 1, 1, .25]),
                                      alphaMode="OPAQUE"))
    document["meshes"][0]["primitives"].append(dict(attributes=attributes,
        indices=len(document["accessors"]) - 1, material=material_index))
    document["buffers"] = [dict(uri="mesh-shadow.bin", byteLength=len(data))]
    (OUT / "mesh-shadow.bin").write_bytes(data)
    (OUT / "mesh-shadow-opaque.gltf").write_text(json.dumps(document), encoding="utf-8")
    document["materials"][material_index].update(alphaMode="MASK", alphaCutoff=.5)
    (OUT / "mesh-shadow-clear.gltf").write_text(json.dumps(document), encoding="utf-8")


def load_capture(path):
    with path.open("rb") as file:
        assert file.readline() == b"P6\n"
        width, height = map(int, file.readline().split())
        assert file.readline() == b"255\n"
        pixels = file.read()
    assert len(pixels) == width * height * 3

    def chunk(kind, value):
        return (struct.pack(">I", len(value)) + kind + value +
                struct.pack(">I", zlib.crc32(kind + value)))

    scanlines = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.with_suffix(".png").write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))
    return width, height, pixels


def region(image, x0, y0, x1, y1):
    width, height, pixels = image
    result = []
    for y in range(int(y0 * height), int(y1 * height)):
        result.extend(pixels[(y * width + int(x0 * width)) * 3:
                             (y * width + int(x1 * width)) * 3])
    return result


def main(overrides=None):
    OUT.mkdir(parents=True, exist_ok=True)
    make_fixture()
    make_surface_contract_fixtures()
    make_mesh_shadow_fixtures()
    config = ROOT / "Data/engine.cfg"
    original = config.read_bytes()
    last = original
    env = os.environ.copy()
    env.update(VK_LAYER_VALIDATE_SYNC="1", VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    results = []
    mesh_shadow_references = {}

    def run(tag, settings, arguments):
        nonlocal last
        settings = {**(overrides or {}), **settings}
        assert config.read_bytes() == last, "Config changed externally; refusing to overwrite it"
        last = original + b"\n" + "".join(f"{k}={v}\n" for k, v in settings.items()).encode()
        config.write_bytes(last)
        log, capture = OUT / f"{tag}.log", OUT / f"{tag}.ppm"
        with log.open("w") as output:
            completed = subprocess.run([str(ROOT / "build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe"), *arguments,
                                        f"--capture={capture}"], cwd=ROOT, env=env,
                                       stdout=output, stderr=subprocess.STDOUT, timeout=180)
        text = log.read_text(errors="replace")
        errors = [line for line in text.splitlines()
                  if "[error]" in line or "VUID-" in line or "SYNC-HAZARD" in line]
        assert completed.returncode == 0 and not errors, f"{tag} failed; see {log}: {errors[:2]}"
        results.append(tag)
        print(f"PASS {tag}", flush=True)
        return load_capture(capture), text

    try:
        for backend in ("geom", "comp"):
            for thread in (0, 1):
                for queue in (0, 1):
                    _, log = run(f"matrix-{backend}-thread{thread}-async{queue}",
                                 {"voxelizer": backend},
                                 ["--smoke-test", f"--rhi-thread={thread}", f"--async-compute={queue}"])
                    revisions = re.findall(r"frame=(\d+) mode=(\d) geometry_revision=(\d+) lighting_revision=(\d+)", log)
                    assert len(revisions) == 48
                    assert len({r[2] for r in revisions[29:]}) == 1, "Light changes rebuilt geometry"
                    assert len({r[3] for r in revisions[35:]}) == 1, "Removed light still animating"

            settings = {"default_model_path": (OUT / "room.gltf").as_posix(),
                        "camera_position": "0,0,1.2", "voxelizer": backend, "voxel_resolution": "64",
                        "light_count": "1", "environment_lighting": "false", "skybox_visible": "false",
                        "light_markers.enabled": "false",
                        "dynamic_light.enabled": "false", "light.0.type": "point",
                        "light.0.position": "0,0.25,0", "light.0.intensity": "0.2", "light.0.range": "3"}
            cases = {
                "point": {}, "direct": {"voxel_gi_indirect_intensity": "0"},
                "dark": {"light_count": "0"},
                "emissive": {"light_count": "0", "default_model_path": (OUT / "emissive.gltf").as_posix()},
                "sky": {"light_count": "0", "environment_lighting": "true", "environment_intensity": "0.25"},
                "missing-sky": {"light_count": "0", "environment_lighting": "true", "environment_texture": "missing-voxel-gi-test.ktx"},
                "directional": {"light.0.type": "directional", "light.0.direction": "0,-1,0.1", "light.0.intensity": "0.5"},
                "spot": {"light.0.type": "spot", "light.0.direction": "0,-1,0", "light.0.outer_angle_degrees": "45", "light.0.inner_angle_degrees": "25"},
            }
            images = {}
            for name, overrides in cases.items():
                images[name], _ = run(f"room-{backend}-{name}", settings | overrides,
                                      ["--frames=3", "--mode=3"])
            assert max(region(images["dark"], .4, .35, .6, .7)) == 0
            assert max(region(images["missing-sky"], .4, .35, .6, .7)) == 0
            point = region(images["point"], .4, .35, .6, .7)
            direct = region(images["direct"], .4, .35, .6, .7)
            assert sum(point) > sum(direct) + 1000, "No indirect contribution on white receiver"
            for x0, x1, channel in ((.39, .43, 0), (.57, .61, 1)):
                bounce = region(images["point"], x0, .5, x1, .66)
                baseline = region(images["direct"], x0, .5, x1, .66)
                delta = [sum(bounce[c::3]) - sum(baseline[c::3]) for c in range(3)]
                assert delta[channel] > max(delta[c] for c in range(3) if c != channel), "No wall color transfer"
            assert sum(region(images["emissive"], .4, .4, .6, .7)) > 1000
            assert sum(region(images["sky"], .4, .35, .6, .7)) > 1000
            assert images["spot"][2] != images["directional"][2]
            # Direction magnitude must not change lighting, even when its float squared length overflows.
            for mode in ((2, 3) if backend == "geom" else (3,)):
                for light_type in ("directional", "spot"):
                    direction_images = {}
                    for name, direction in (("unit", "0,0,-1"), ("large", "0,0,-1e20")):
                        direction_images[name], _ = run(
                            f"direction-{backend}-mode{mode}-{light_type}-{name}",
                            settings | {"light.0.type": light_type, "light.0.direction": direction,
                                        "light.0.intensity": "0.5"},
                            ["--frames=3", f"--mode={mode}"])
                    assert sum(region(direction_images["unit"], .4, .35, .6, .7)) > 1000, \
                        "Direction reference receiver is not lit"
                    assert direction_images["unit"] == direction_images["large"], \
                        "Large finite direction changes lighting"
            # Equivalent material factors and vertex colors must produce identical output.
            # A 0.7 mask alpha must survive a 0.5 cutoff, just like fully opaque coverage.
            for mode in ((2, 3) if backend == "geom" else (3,)):
                material_images = {}
                for variant in ("factor", "vertex", "masked", "opaque", "vertex-rgba",
                                "transparent", "transparent-rgba"):
                    material_images[variant], _ = run(
                        f"material-{backend}-mode{mode}-{variant}",
                        settings | {"default_model_path": (OUT / f"material-{variant}.gltf").as_posix()},
                        ["--frames=3", f"--mode={mode}"])
                assert material_images["factor"] == material_images["vertex"], "Base color applied more than once"
                assert material_images["factor"] == material_images["vertex-rgba"], "RGBA vertex colors decoded incorrectly"
                assert material_images["masked"] == material_images["opaque"], "Valid masked surfaces disappeared"
                assert material_images["transparent"] == material_images["transparent-rgba"], "Vertex alpha was ignored"
                assert max(material_images["transparent-rgba"][2]) == 0, "Zero-alpha masked surfaces remained visible"
            for mode in ((2, 3) if backend == "geom" else (3,)):
                reference = images["point"] if mode == 3 else run(
                    "surface-geom-mode2-reference", settings, ["--frames=3", "--mode=2"])[0]
                surfaces = {}
                for name in ("padded", "no-normals", "default-explicit", "default-omitted", "default-only",
                             "texture-split", "texture-shared-image", "texture-shared-slot"):
                    overrides = {"default_model_path": (OUT / f"surface-{name}.gltf").as_posix()}
                    if name.startswith("default-"):
                        overrides["light_count"] = "0"
                    surfaces[name], _ = run(f"surface-{backend}-mode{mode}-{name}", settings | overrides,
                                             ["--frames=3", f"--mode={mode}"])
                assert surfaces["padded"] == reference, "Vertex attribute stride changes geometry"
                assert surfaces["no-normals"] == reference, "Missing normals differ from flat normals"
                assert surfaces["default-explicit"] == surfaces["default-omitted"] == surfaces["default-only"], \
                    "Missing material does not use the initialized default material"
                assert max(region(surfaces["default-omitted"], .4, .4, .6, .6)) == 0
                assert surfaces["texture-split"] == surfaces["texture-shared-image"] == surfaces["texture-shared-slot"], \
                    "Image/texture sharing changes material color-space interpretation"
            for light_type in ("point", "spot"):
                shadow_settings = settings | {"light.0.type": light_type, "light.0.position": "0,0,-0.455",
                    "light.0.direction": "0,0,-1", "voxel_gi_indirect_intensity": "0",
                    "voxel_gi_normal_bias_voxels": "1.5", "voxel_gi_shadow_enabled": "true"}
                center_regions = {}
                for name in ("room", "shadow-behind", "shadow-before"):
                    capture, _ = run(f"endpoint-{backend}-{light_type}-{name}",
                        shadow_settings | {"default_model_path": (OUT / f"{name}.gltf").as_posix()},
                        ["--frames=3", "--mode=3"])
                    center_regions[name] = region(capture, .49, .49, .51, .51)
                assert sum(center_regions["room"]) > 1000, "Shadow reference receiver is not lit"
                assert center_regions["room"] == center_regions["shadow-behind"], \
                    "Blocker beyond the light casts a shadow"
                assert max(center_regions["shadow-before"]) == 0, "Blocker before the light failed to cast a shadow"
            # Direct mesh shadows must retain the slanted edge independently of voxel
            # resolution/backend, respect cutoff alpha, and honor per-light shadow disable.
            for light_type in ("point", "spot", "directional"):
                shadow_settings = settings | {"light.0.type": light_type,
                    "light.0.position": "0.1,0.1,0.2", "light.0.direction": "0,0,-1",
                    "light.0.inner_angle_degrees": "45", "light.0.outer_angle_degrees": "60",
                    "voxel_gi_indirect_intensity": "0", "voxel_gi_shadow_enabled": "true"}
                shadow_images = {}
                for resolution in (64, 128, 256):
                    shadow_images[resolution], _ = run(f"mesh-shadow-{backend}-{light_type}-{resolution}",
                        shadow_settings | {"default_model_path": (OUT / "mesh-shadow-opaque.gltf").as_posix(),
                                           "voxel_resolution": str(resolution)}, ["--frames=3", "--mode=3"])
                assert shadow_images[64] == shadow_images[128] == shadow_images[256], \
                    "Direct shadows still depend on voxel resolution"
                if backend == "geom":
                    mesh_shadow_references[light_type] = shadow_images[64]
                else:
                    assert shadow_images[64] == mesh_shadow_references[light_type], \
                        "Direct shadows still depend on the voxelizer backend"
                clear, _ = run(f"mesh-shadow-{backend}-{light_type}-cutout", shadow_settings |
                    {"default_model_path": (OUT / "mesh-shadow-clear.gltf").as_posix()}, ["--frames=3", "--mode=3"])
                unshadowed, _ = run(f"mesh-shadow-{backend}-{light_type}-disabled", shadow_settings |
                    {"default_model_path": (OUT / "mesh-shadow-opaque.gltf").as_posix(),
                     "light.0.casts_shadows": "false"}, ["--frames=3", "--mode=3"])
                assert sum(region(clear, .35, .35, .65, .65)) > \
                    sum(region(shadow_images[64], .35, .35, .65, .65)) + 1000, "Triangle did not cast a shadow"
                # The room's ceiling panel still casts a real shadow in the cutout case.
                # Compare only the back-wall receiver covered by the triangle's shadow.
                assert region(clear, .42, .34, .58, .68) == region(unshadowed, .42, .34, .58, .68), \
                    "Alpha cutout, shadow disable, or receiver-plane filtering is incorrect"

            animated = settings | {"dynamic_light.enabled": "true", "dynamic_light.index": "0",
                                   "dynamic_light.orbit_center": "0,0.25,0", "dynamic_light.orbit_radius": "0.25"}
            lit, _ = run(f"room-{backend}-dynamic-on", animated, ["--smoke-test", "--frames=34"])
            removed, _ = run(f"room-{backend}-dynamic-removed", animated, ["--smoke-test", "--frames=35"])
            assert sum(region(lit, .4, .35, .6, .7)) > 1000
            assert max(region(removed, .4, .35, .6, .7)) == 0, "Removed light left residual output"

            # Tiny light range isolates visible markers from illumination of the room walls.
            markers = settings | {"light_markers.enabled": "true", "light_markers.size": "0.06",
                                  "light.0.position": "0,0,0", "light.0.color": "0.2,0.4,1",
                                  "light.0.intensity": "2", "light.0.range": "0.01"}
            for mode in ((2, 3) if backend == "geom" else (3,)):
                arguments = ["--frames=3", f"--mode={mode}"]
                prefix = f"markers-{backend}-mode{mode}"
                off, _ = run(f"{prefix}-off", markers | {"light_markers.enabled": "false"}, arguments)
                on, _ = run(f"{prefix}-on", markers, arguments)
                assert max(region(off, .3, .3, .7, .7)) == 0, "Marker fixture has unexpected surface lighting"
                assert sum(region(on, .45, .45, .55, .55)) > 1000, "Point-light box is missing"
                center = region(on, .45, .45, .55, .55)
                assert sum(center[2::3]) > sum(center[1::3]) > sum(center[0::3]), "Marker lost its light color"
                occluded, _ = run(f"{prefix}-occluded", markers | {"light.0.position": "0,0,-0.7"}, arguments)
                assert occluded == off, "Light marker is visible through the back wall"
                disabled, _ = run(f"{prefix}-disabled", markers | {"light.0.enabled": "false"}, arguments)
                assert disabled == off, "Disabled light still has a marker"
                zero, _ = run(f"{prefix}-zero", markers | {"light.0.intensity": "0"}, arguments)
                assert zero == off, "Zero-intensity light still has a marker"
                spot, _ = run(f"{prefix}-spot", markers | {"light.0.type": "spot"}, arguments)
                assert spot == on, "Spot-light source marker differs from the equivalent point light"
                direction = markers | {"light.0.type": "directional"}
                direction_off, _ = run(f"{prefix}-direction-off", direction | {"light_markers.enabled": "false"}, arguments)
                direction_on, _ = run(f"{prefix}-direction-on", direction, arguments)
                assert direction_on == direction_off, "Directional light incorrectly has a finite source box"

            moving_markers = markers | {"dynamic_light.enabled": "true", "dynamic_light.index": "0",
                                        "dynamic_light.orbit_center": "0,0,0", "dynamic_light.orbit_radius": "0.2",
                                        "dynamic_light.angular_speed_degrees": "90"}
            first, _ = run(f"markers-{backend}-moving-first", moving_markers, ["--smoke-test", "--frames=33"])
            second, _ = run(f"markers-{backend}-moving-second", moving_markers, ["--smoke-test", "--frames=34"])
            gone, _ = run(f"markers-{backend}-removed", moving_markers, ["--smoke-test", "--frames=35"])
            first_marker = region(first, .3, .3, .7, .7)
            second_marker = region(second, .3, .3, .7, .7)
            assert sum(first_marker) > 1000 and sum(second_marker) > 1000, "Dynamic light marker is missing"
            assert first_marker != second_marker, "Dynamic light marker did not move"
            assert max(region(gone, .3, .3, .7, .7)) == 0, "Removed dynamic light left a marker"
        (OUT / "results.json").write_text(json.dumps({"passed": results}, indent=2))
    finally:
        if config.read_bytes() == last:
            config.write_bytes(original)
        else:
            print("Config changed externally; leaving it untouched. Original saved to config.original.")
            (OUT / "config.original").write_bytes(original)
    print(f"All {len(results)} GPU cases and image assertions passed. Captures: {OUT}")


if __name__ == "__main__":
    main()

