"""Make Android-built asset bundles render on iOS (Metal) by swapping shaders.

Shaders inside the Android bundles only carry GLES/Vulkan programs, so on
iOS every material using them renders magenta. The iOS player data
(Data/data.unity3d in the IPA) holds Metal builds of the shaders compiled
into the player. For every Shader object in a bundle:

* same name exists in the IPA  -> replace with the IPA's Metal shader;
* otherwise, a mapped stand-in -> replace with that IPA shader (approximate
  look: no toon shading, lightmaps or special blends);
* unmapped                      -> left as is (still magenta).

Cross-file references inside the copied shader (dependencies, default
textures) point into the player data and are cleared. Materials that keep
their texture only in _BaseMap get it mirrored into _MainTex so built-in
unlit stand-ins can sample it. The result is also retargeted to the iOS
build target (unityfs_retarget).

Requires UnityPy (pip install UnityPy) and lz4.
"""
from __future__ import annotations

import copy
import io
import sys
from pathlib import Path

import UnityPy

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    from .unityfs_retarget import retarget  # noqa: E402
except ImportError:
    from unityfs_retarget import retarget  # noqa: E402

EMPTY = "Hidden/VFX/Empty"
STAND_INS = {
    "Ip0013/BG/unlit_lightmap_uv2_cull": "Unlit/Texture",
    "Universal Render Pipeline/Unlit": "Unlit/Texture",
    "MELPOT/Toon/UberToonShader": "Unlit/Texture",
    "MELPOT/Toon/UberToonShader(HLSLMacros)": "Unlit/Texture",
    "MELPOT/Toon/UberToonShader_Eye": "Unlit/Transparent",
    "Character/ToonShader_CharacterEye": "Unlit/Transparent",
    "Character/ToonShader_CharacterHighlight": EMPTY,
    "Character/Highlight_Distortion": EMPTY,
    "Mobile/Particles/Multiply": EMPTY,
    "Spine/Skeleton": "Legacy Shaders/Particles/Alpha Blended Premultiply",
    "Spine/Blend Modes/Skeleton PMA Multiply": "Legacy Shaders/Particles/Alpha Blended Premultiply",
    "Spine/Blend Modes/Skeleton PMA Additive": "Mobile/Particles/Additive",
    "Spine/SkeletonGraphic": "UI/Default",
    "Spine/SkeletonGraphic Multiply": "UI/Default",
    "Spine/SkeletonGraphic Screen": "UI/Default",
    "Spine/SkeletonGraphic Additive": "UI/Additive",
}


def load_player_shaders(data_unity3d: Path) -> dict[str, dict]:
    env = UnityPy.load(str(data_unity3d))
    shaders: dict[str, dict] = {}
    for obj in env.objects:
        if obj.type.name != "Shader":
            continue
        tree = obj.read_typetree()
        name = tree["m_ParsedForm"]["m_Name"]
        if 14 not in (tree.get("platforms") or []):
            continue
        tree["m_Dependencies"] = []
        if "m_NonModifiableTextures" in tree:
            tree["m_NonModifiableTextures"] = []
        shaders.setdefault(name, tree)
    return shaders


def _mirror_base_map(tree: dict) -> bool:
    envs = tree["m_SavedProperties"]["m_TexEnvs"]
    table = dict((k, v) for k, v in envs)
    base = table.get("_BaseMap")
    if not base or not base["m_Texture"]["m_PathID"]:
        return False
    main = table.get("_MainTex")
    if main and main["m_Texture"]["m_PathID"]:
        return False
    entry = copy.deepcopy(base)
    rebuilt = [(k, entry if k == "_MainTex" else v) for k, v in envs]
    if main is None:
        rebuilt.append(("_MainTex", entry))
    envs[:] = rebuilt
    return True


def swap(blob: bytes, player: dict[str, dict]) -> tuple[bytes, dict]:
    prefix = blob.find(b"UnityFS")
    env = UnityPy.load(blob[prefix:])
    stats = {"metal": 0, "stand_in": 0, "unmapped": 0, "materials": 0}
    touched = False
    for obj in env.objects:
        if obj.type.name == "Shader":
            tree = obj.read_typetree()
            name = tree["m_ParsedForm"]["m_Name"]
            if name in player:
                source, kind = player[name], "metal"
            elif STAND_INS.get(name) in player:
                source, kind = player[STAND_INS[name]], "stand_in"
            else:
                stats["unmapped"] += 1
                continue
            new = copy.deepcopy(source)
            new["m_ParsedForm"]["m_Name"] = name
            obj.save_typetree(new)
            stats[kind] += 1
            touched = True
        elif obj.type.name == "Material":
            tree = obj.read_typetree()
            if _mirror_base_map(tree):
                obj.save_typetree(tree)
                stats["materials"] += 1
                touched = True
    if not touched:
        return retarget(blob)[0], stats
    saved = env.file.save(packer="lz4")
    return retarget(blob[:prefix] + saved)[0], stats


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: unityfs_shader_swap.py DATA_UNITY3D IN OUT")
        return 2
    player = load_player_shaders(Path(argv[1]))
    out, stats = swap(Path(argv[2]).read_bytes(), player)
    Path(argv[3]).write_bytes(out)
    print(stats)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
