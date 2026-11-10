"""Copies the renderer of Epic's GaussianSplat plugin into the VGS plugin, renamed.

    python plugins/unreal/tools/copy_epic_splat_renderer.py <engine root> [<output plugin dir>]

    e.g. python plugins/unreal/tools/copy_epic_splat_renderer.py E:/UE_INSTALL/UE_5.9

This is the mechanical half of EPIC_RENDERER_COPY.md (beside the plugin): it copies the
GaussianSplatSceneViewExtension module and the shaders it compiles, renames everything that
would collide with Epic's own copy when both are loaded (module, classes, shader types and
paths, console variables, stats, log category), and leaves untouched what is still Epic's
and shared with their plugin (the GaussianSplat runtime module's types, the shared shader
headers, the inverse tone curve LUT assets). It does not apply the VGS changes; those are
in vgs_renderer_changes.patch, applied after it (see the .md).

Output: <plugin>/Source/VGSSplatRenderer and <plugin>/Shaders/Private/SplatRenderer.
Both are replaced whole.
"""

import os
import re
import shutil
import sys

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "E:/UE_INSTALL/UE_5.9"
PLUGIN = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "VGS")
EPIC = os.path.join(ENGINE, "Engine", "Plugins", "GaussianSplat")

SOURCE_IN = os.path.join(EPIC, "Source", "GaussianSplatSceneViewExtension")
SOURCE_OUT = os.path.join(PLUGIN, "Source", "VGSSplatRenderer")
SHADERS_OUT = os.path.join(PLUGIN, "Shaders", "Private", "SplatRenderer")

# Files of the module not copied: they register Epic's own components with the renderer,
# and this copy must never draw those (Epic's renderer does).
SKIPPED = {"GaussianSplatSceneViewExtensionRenderer.h", "GaussianSplatSceneViewExtensionRenderer.cpp"}

# Shaders the module compiles; Niagara's are Epic's business.
SHADERS = [
    "Private/GaussianSplatCommon.ush",
    "Private/GaussianSplatCullClusters.usf",
    "Private/GaussianSplatDownsample.usf",
    "Private/GaussianSplatDrawIndirectArgs.usf",
    "Private/GaussianSplatFillSplatIndexBuffer.usf",
    "Private/GaussianSplatGatherShadedSplats.usf",
    "Private/GaussianSplatHardwareRasterize.usf",
    "Private/GaussianSplatInverseToneCurvePostBlend.usf",
    "Private/GaussianSplatPrefixSum.usf",
    "Private/GaussianSplatRasterization.usf",
    "Private/GaussianSplatShadeIndirectArgs.usf",
    "Private/GaussianSplatShading.usf",
    "Private/GaussianSplatTileCount.usf",
    "Private/GaussianSplatTileRasterize.usf",
    "Public/SphericalGaussianCommon.ush",
]

# Still Epic's, used as they are: the GaussianSplat runtime module's types and headers,
# the shared shader headers, the LUT assets. Protected from the renaming below.
KEEP = [
    r'"/Plugin/GaussianSplat/Shared/',
    r'/GaussianSplat/LUTs/',
    r'"GaussianSplat\.h"',
    r'"GaussianSplatCommon\.h"',
    r'"GaussianSplatCluster\.h"',
    r'"GaussianSplatComponent\.h"',
    r'"GaussianSplatRenderResource\.h"',
    r'"GaussianSplatRenderer\.h"',
    r'\bGaussianSplat::IsEnabled\b',
    r'\bGaussianSplatShared\b',
    r'\bGaussianSplatDefinitions\b',
    r'\bFGaussianSplatShared\b',
    r'\bFGaussianSplatSHConstants\b',
    r'\bFGaussianSplatCluster\b',
    r'\bFGaussianSplatFormatQuantized\b',
    r'\bFGaussianSplatRenderResource\b',
    r'\bFGaussianSplatRendererRegistry\b',
    r'\bIGaussianSplatRenderer\b',
    r'\bEGaussianSplatCoordinateSpace\b',
    r'\bUGaussianSplatComponent\b',
    r'\bUGaussianSplat\b',
    r'"GaussianSplat"',  # dependency module names in Build.cs
    r'"GaussianSplatShader"',
]

# Shader types whose names do not say GaussianSplat: renamed by hand, since two global
# shader types of one name stop the engine.
SHADER_TYPES = ["FCullClustersCS", "FInverseToneCurvePostBlendPS", "FHardwareRasterizeStochasticDepthPS"]

RENAMES = [
    (r'GAUSSIANSPLATSCENEVIEWEXTENSION_API', 'VGSSPLATRENDERER_API'),
    (r'GaussianSplatSceneViewExtension', 'VGSSplatRenderer'),
    (r'/Plugin/GaussianSplat/Private/', '/Plugin/VGS/Private/SplatRenderer/'),
    (r'/Plugin/GaussianSplat/Public/', '/Plugin/VGS/Private/SplatRenderer/'),
    (r'r\.GaussianSplat\.', 'r.VGSSplat.'),
    (r'GaussianSplat', 'VGSSplat'),
] + [(r'\b' + name + r'\b', 'FVGSSplat' + name[1:]) for name in SHADER_TYPES]

HEADER = ("// VGS: a copy of Epic's GaussianSplat renderer (UE {version}), renamed by "
          "plugins/unreal/tools/copy_epic_splat_renderer.py.\n"
          "// What VGS changed is marked \"VGS CHANGE\"; see plugins/unreal/EPIC_RENDERER_COPY.md.\n")


def engine_version():
    import json
    with open(os.path.join(ENGINE, "Engine", "Build", "Build.version")) as f:
        v = json.load(f)
    return "%d.%d.%d, CL %s" % (v["MajorVersion"], v["MinorVersion"], v["PatchVersion"], v.get("Changelist", "?"))


def transform(text):
    kept = []

    def protect(match):
        kept.append(match.group(0))
        return "\x00%d\x00" % (len(kept) - 1)

    for pattern in KEEP:
        text = re.sub(pattern, protect, text)
    for pattern, replacement in RENAMES:
        text = re.sub(pattern, replacement, text)
    return re.sub("\x00(\\d+)\x00", lambda m: kept[int(m.group(1))], text)


def write(path, text, header):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header + text)


def main():
    header = HEADER.format(version=engine_version())
    for out in (SOURCE_OUT, SHADERS_OUT):
        if os.path.isdir(out):
            shutil.rmtree(out)

    for root, _, files in os.walk(SOURCE_IN):
        for name in files:
            if name in SKIPPED:
                continue
            with open(os.path.join(root, name), encoding="utf-8-sig") as f:
                text = f.read()
            relative = os.path.relpath(os.path.join(root, name), SOURCE_IN)
            target = os.path.join(SOURCE_OUT, transform(relative))
            # Build.cs files are C#: the same comment syntax, so the same header.
            write(target, transform(text), header)

    for shader in SHADERS:
        with open(os.path.join(EPIC, "Shaders", shader), encoding="utf-8-sig") as f:
            text = f.read()
        target = os.path.join(SHADERS_OUT, transform(os.path.basename(shader)))
        write(target, transform(text), header)

    print("copied into", os.path.abspath(SOURCE_OUT), "and", os.path.abspath(SHADERS_OUT))


if __name__ == "__main__":
    main()
