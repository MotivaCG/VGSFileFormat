"""Packs Epic's inverse tone curve LUTs into the files the splat renderer copy loads at run time.

    python plugins/unreal/tools/pack_inverse_tone_curve_luts.py <dir with the .raw dumps>

Epic's renderer samples two 3D LUTs, `/GaussianSplat/LUTs/UE_InverseFilmicTC` and
`UE_InverseFilmicTC_Full`, which are UVolumeTexture assets of Epic's GaussianSplat plugin:
saved by Unreal 5.9 and missing before it. The copy builds the same textures at run time,
on every engine version, from these files instead (EPIC_RENDERER_COPY.md, "The LUTs").

Input: each asset's source mip 0 as written by `FTextureSource::GetMipData` (65x65x65,
RGBA16F, x fastest, then y, then z), named `<asset>.raw`; EPIC_RENDERER_COPY.md has the
console command that dumps them. Alpha is 1 everywhere and is dropped.

Output, in plugins/unreal/VGS/Resources/InverseToneCurve/<asset>.vgslut:

    char[4]  "VLUT"
    uint32   edge            (65)
    uint32   channels        (3: RGB, half floats)
    uint32   unpacked bytes  (edge^3 * channels * 2)
    ...      zlib stream of the half floats, x fastest
"""

import os
import struct
import sys
import zlib

import numpy as np

ASSETS = ["UE_InverseFilmicTC", "UE_InverseFilmicTC_Full"]
EDGE = 65
OUT = os.path.join(os.path.dirname(__file__), "..", "VGS", "Resources", "InverseToneCurve")


def main():
    source = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    for name in ASSETS:
        raw = np.fromfile(os.path.join(source, name + ".raw"), dtype=np.float16)
        rgba = raw.reshape(EDGE, EDGE, EDGE, 4)
        if not np.all(rgba[..., 3] == 1.0):
            raise SystemExit(name + ": alpha is not 1 everywhere; the format would lose it")
        rgb = np.ascontiguousarray(rgba[..., :3]).tobytes()
        packed = zlib.compress(rgb, 9)
        with open(os.path.join(OUT, name + ".vgslut"), "wb") as f:
            f.write(b"VLUT" + struct.pack("<III", EDGE, 3, len(rgb)))
            f.write(packed)
        print(name, len(rgb), "->", len(packed) + 16, "bytes")


if __name__ == "__main__":
    main()
