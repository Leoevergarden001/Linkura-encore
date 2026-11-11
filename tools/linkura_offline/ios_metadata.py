"""IL2CPP metadata v31 identities and statically recovered CodeGenModule pointers.

Structure reference: Perfare/Il2CppDumper MetadataClass.cs and Il2CppClass.cs.
Tokens/addresses identify candidates only; calling conventions and runtime hook
feasibility must still be verified on a device.
"""
from __future__ import annotations

from .ios_macho import IosError, MachO, cstring, region, unpack


TARGETS = (
    ("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "AddDefaultHeader"),
    ("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "set_UserAgent"),
    ("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "set_BasePath"),
    ("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "ApiClient", "CallApiAsync"),
    ("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "ApiClient", "Deserialize"),
    ("Core.dll", "", "Core", "SynchronizeResourceVersion"),
    ("UnityEngine.CoreModule.dll", "UnityEngine", "Application", "get_version"),
)


class Metadata31:
    def __init__(self, data: bytes):
        magic, version = unpack(data, "<II", 0)
        if magic != 0xFAB11BAF or version != 31:
            raise IosError("metadata method mapping currently requires IL2CPP metadata v31")
        self.data = data
        self.strings = self.section(2)
        self.methods = self.section(5, 36)
        self.types = self.section(19, 88)
        self.images = self.section(20, 40)

    def section(self, index: int, stride: int = 1) -> bytes:
        offset, size = unpack(self.data, "<II", 8 + index * 8)
        if size % stride:
            raise IosError(f"misaligned metadata section {index}")
        return region(self.data, offset, size)

    def string(self, index: int) -> str:
        return cstring(self.strings, index)

    def target_methods(self) -> list[dict]:
        result = []
        for index in range(len(self.images) // 40):
            name, _, start, count, *_ = unpack(self.images, "<IiiIIiIIII", index * 40)
            image = self.string(name)
            wanted = [t for t in TARGETS if t[0] == image]
            if not wanted:
                continue
            region(self.types, start * 88, count * 88)
            for ti in range(start, start + count):
                record = region(self.types, ti * 88, 88)
                type_name, namespace = map(self.string, unpack(record, "<II", 0))
                selected = [t for t in wanted if t[1:3] == (namespace, type_name)]
                if not selected:
                    continue
                method_start = unpack(record, "<i", 36)[0]
                method_count = unpack(record, "<H", 64)[0]
                region(self.methods, method_start * 36, method_count * 36)
                for mi in range(method_start, method_start + method_count):
                    method = region(self.methods, mi * 36, 36)
                    name, declaring, _, _, _, _, token, flags, _, _, parameter_count = unpack(method, "<IiiiiiIHHHH", 0)
                    method_name = self.string(name)
                    if method_name not in [t[3] for t in selected]:
                        continue
                    if declaring != ti or token >> 24 != 6 or not token & 0xFFFFFF:
                        raise IosError("invalid metadata method owner/token")
                    result.append(dict(image=image, namespace=namespace, type=type_name, method=method_name,
                                       typeIndex=ti, methodIndex=mi, token=f"0x{token:08x}",
                                       parameterCount=parameter_count, isStatic=bool(flags & 0x10)))
        return result


def map_methods(metadata: Metadata31, image: MachO) -> list[dict]:
    targets = metadata.target_methods()
    refs = image.references()
    modules: dict[str, list[tuple[int, int, int]]] = {}
    for name in sorted({t["image"] for t in targets}):
        candidates = []
        cursor = 0
        needle = name.encode() + b"\0"
        while (cursor := image.data.find(needle, cursor)) >= 0:
            try:
                address = image.file_to_vm(cursor)
                for position in refs.get(address, []):
                    count = unpack(image.data, "<Q", position + 8)[0]
                    table = image.pointer(position + 16)
                    if 0 < count <= len(metadata.methods) // 36 and table:
                        table_offset = image.vm_to_file(table, count * 8)
                        candidates.append((position, count, table_offset))
            except IosError:
                pass
            cursor += len(needle)
        modules[name] = sorted(set(candidates))
    for target in targets:
        candidates = modules[target["image"]]
        target.update(addressStatus="unresolved", imageRelativeAddress=None, fileOffset=None)
        if len(candidates) != 1:
            target["reason"] = f"expected one CodeGenModule, found {len(candidates)}"
            continue
        module, count, table = candidates[0]
        rid = int(target["token"], 16) & 0xFFFFFF
        if rid > count:
            raise IosError("method token exceeds CodeGenModule pointer count")
        address = image.pointer(table + (rid - 1) * 8)
        target["codeGenModuleFileOffset"] = hex(module)
        if not address:
            target["reason"] = "null method pointer (abstract, generic, or runtime-provided)"
            continue
        if not image.is_code(address) or address % 4:
            raise IosError("recovered method pointer is outside executable instructions")
        offset = image.vm_to_file(address, 16)
        target.update(addressStatus="static-pointer-resolved", imageRelativeAddress=hex(address - image.base),
                      fileOffset=hex(offset), first16Bytes=region(image.data, offset, 16).hex(),
                      runtimeValidated=False)
    return targets
