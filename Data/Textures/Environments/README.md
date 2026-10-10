# Environment starter set

Eight Radiance HDR panoramas from Poly Haven, about 14.9 MB in total: seven
1024 × 512 images and one 2048 × 1024 clear-noon sky for sun-shadow comparisons.
They are optional and not stored in the repository. Download them into this folder,
verified against `sources.json`, with:

```bash
python tools/fetch_environments.py
```

The files retain their original linear HDR values. Tests never need them: they
generate small panoramas at runtime.

| File | Lighting | Creator / source |
| --- | --- | --- |
| `kloppenheim_06_puresky_1k.hdr` | Soft sunrise sky | [Greg Zaal; sky edits by Jarod Guest](https://polyhaven.com/a/kloppenheim_06_puresky) |
| `kloofendal_48d_partly_cloudy_puresky_1k.hdr` | Midday sun with scattered clouds | [Greg Zaal; sky edits by Jarod Guest](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky) |
| `qwantani_noon_puresky_2k.hdr` | Clear midday sky with a bright sun and high contrast | [Greg Zaal; processing by Jarod Guest](https://polyhaven.com/a/qwantani_noon_puresky) |
| `studio_small_09_1k.hdr` | Indoor studio softboxes | [Sergej Majboroda](https://polyhaven.com/a/studio_small_09) |
| `small_empty_room_1_1k.hdr` | Soft window daylight in an empty room | [Sergej Majboroda](https://polyhaven.com/a/small_empty_room_1) |
| `hotel_room_1k.hdr` | Hotel bedroom with window light and warm lamps | [Greg Zaal](https://polyhaven.com/a/hotel_room) |
| `large_corridor_1k.hdr` | Sunlit corridor with arched windows | [Sergej Majboroda](https://polyhaven.com/a/large_corridor) |
| `carpentry_shop_01_1k.hdr` | Workshop with artificial lighting and doorway daylight | [Greg Zaal](https://polyhaven.com/a/carpentry_shop_01) |

All assets are [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/),
as described in [Poly Haven's asset license](https://polyhaven.com/license).
Redistribution and commercial use are permitted. Attribution is included for
provenance; it is not required by CC0. `sources.json` records the download URLs,
byte counts and SHA-256 checksums. The editor needs no network access; only the
fetch script downloads.

In ZenEditor, open a scene and select **Render Settings → Environment**. The
dropdown also includes the existing `papermill.ktx`. Add more `.hdr`, `.ktx` or
`.dds` files under `Data/Textures` and click **Refresh list**, or use **Browse**
to load a texture outside the repository. File discovery filters extensions;
the renderer validates the selected file's contents.

The loader accepts 2:1 Radiance HDR panoramas up to 8192 × 4096 and floating-point
RGBA16F/RGBA32F KTX/DDS cubemaps. Panoramas are converted to six linear RGBA32F
faces, at one quarter of panorama width per face, capped at 512 pixels. The
existing RDG/RHI passes generate diffuse irradiance and prefiltered reflections.
File cubemaps use the engine's existing inverted-Y source convention; HDR
conversion applies this automatically. Cubemaps must be non-array, little-endian
KTX1 or DDS, at most 4096 pixels per face and 256 MB per file. Container headers
and payload sizes are validated before handing them to GLI.
