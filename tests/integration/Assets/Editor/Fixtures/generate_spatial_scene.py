"""Generate an original single-level spatial fixture from the shared panel scene."""
import json
from pathlib import Path
from generate_array_scene import generate

def spatial():
    path = Path(__file__).with_name("spatial_scene.gltf")
    generate(128, path)
    scene = json.loads(path.read_text(encoding="utf-8"))
    scene["asset"]["generator"] = "ZenEngine generate_spatial_scene.py (original test fixture)"
    for sampler in scene["samplers"]:
        sampler["minFilter"] = 9729
    scene["extensionsUsed"].append("KHR_materials_clearcoat")
    for index in range(2):
        scene["materials"][index].setdefault("extensions", {})["KHR_materials_clearcoat"] = {
            "clearcoatFactor": 0.5, "clearcoatRoughnessFactor": 0.3,
            "clearcoatNormalTexture": {"index": index + 2, "scale": 0.5}}
    path.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__":
    spatial()
