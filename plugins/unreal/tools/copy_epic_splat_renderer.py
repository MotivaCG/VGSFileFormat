"""Copies the renderer of Epic's GaussianSplat plugin into the VGS plugin, renamed.

    python plugins/unreal/tools/copy_epic_splat_renderer.py <engine root> [<output plugin dir>]

    e.g. python plugins/unreal/tools/copy_epic_splat_renderer.py E:/UE_INSTALL/UE_5.9

This is the mechanical half of EPIC_RENDERER_COPY.md (beside the plugin): it copies the
GaussianSplatSceneViewExtension module and the shaders it compiles, renames everything that
would collide with Epic's own copy when both are loaded (module, classes, shader types and
paths, console variables, stats, log category). It also copies, renamed the same way, the
few things of Epic's runtime module and shared shader headers the renderer uses (the
coordinate space enum, the cluster record, the record layout and constants), so the copy
does not need Epic's GaussianSplat plugin, which only Unreal 5.9 has. What it leaves
untouched (Epic's component and asset types, the LUT assets) the VGS changes remove. It
does not apply the VGS changes; those are in vgs_renderer_changes.patch, applied after it
(see the .md).

Output: <plugin>/Source/VGSSplatRenderer, <plugin>/Shaders/Private/SplatRenderer and
<plugin>/Shaders/Shared. All three are replaced whole.
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
SHARED_OUT = os.path.join(PLUGIN, "Shaders", "Shared")

# From Epic's GaussianSplat runtime module, into the copy's Internal/: what the renderer
# needs of it.
RUNTIME_HEADERS = ["GaussianSplatCommon.h", "GaussianSplatCluster.h"]

# Epic's shared shader headers, read by both C++ and HLSL: into Shaders/Shared, which the
# copy's Build.cs adds to its include path and /Plugin/VGS/Shared/ maps for shaders.
SHARED_HEADERS = ["GaussianSplatDefinitions.h", "GaussianSplatShared.h"]

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

# Epic's, left as they are: the code of the copy that draws Epic's own components and loads
# Epic's LUT assets, which the VGS changes remove (the copy draws only its own sources and
# builds the LUTs from Resources/InverseToneCurve). Protected from the renaming below, so
# that what the changes remove is plainly Epic's.
KEEP = [
    r'/GaussianSplat/LUTs/',
    r'"GaussianSplat\.h"',
    r'"GaussianSplatComponent\.h"',
    r'"GaussianSplatRenderResource\.h"',
    r'"GaussianSplatRenderer\.h"',
    r'\bGaussianSplat::IsEnabled\b',
    r'\bFGaussianSplatFormatQuantized\b',
    r'\bFGaussianSplatRenderResource\b',
    r'\bFGaussianSplatRendererRegistry\b',
    r'\bIGaussianSplatRenderer\b',
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
    (r'/Plugin/GaussianSplat/Shared/', '/Plugin/VGS/Shared/'),
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
    for out in (SOURCE_OUT, SHADERS_OUT, SHARED_OUT):
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

    for name in RUNTIME_HEADERS:
        with open(os.path.join(EPIC, "Source", "GaussianSplat", "Internal", name), encoding="utf-8-sig") as f:
            text = f.read()
        write(os.path.join(SOURCE_OUT, "Internal", transform(name)), transform(text), header)

    for name in SHARED_HEADERS:
        with open(os.path.join(EPIC, "Shaders", "Shared", name), encoding="utf-8-sig") as f:
            text = f.read()
        write(os.path.join(SHARED_OUT, transform(name)), transform(text), header)

    print("copied into", os.path.abspath(SOURCE_OUT), ",", os.path.abspath(SHADERS_OUT), "and", os.path.abspath(SHARED_OUT))


if __name__ == "__main__":
    main()
