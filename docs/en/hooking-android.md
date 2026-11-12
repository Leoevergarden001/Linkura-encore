<!-- Translation source: 60f383ea76a885c71ddad0e2d24e2955fc4c6c78 (docs/zh/hooking-android.md) -->

# Android hooks

The native module uses ShadowHook for inline hooks and UnityResolve and the IL2CPP API to find classes and methods **by name**, so it does not depend on fixed addresses. Minor client updates usually require no address changes.

## Write a hook

```cpp
// hooks/HookShare.cpp or hooks/HookOfflineFixes.cpp
DEFINE_HOOK(int32_t, CommonApiCache_get_CurrentChapterRank, (void* self, void* mtd)) {
    const auto rank = CommonApiCache_get_CurrentChapterRank_Orig(self, mtd);
    return rank;
}

void Install(HookInstaller* hookInstaller) {
    ADD_HOOK(CommonApiCache_get_CurrentChapterRank, Il2cppUtils::GetMethodPointer(
            "Assembly-CSharp.dll", "Tecotec", "CommonApiCache", "get_CurrentChapterRank"));
}
```

- `DEFINE_HOOK(return_type, name, (args...))` generates `name_Hook` and the original-function pointer `name_Orig`. The first argument of an instance method is `self` and the last is `MethodInfo*`.
- `ADD_HOOK(name, address)` installs the hook; logs include `ADD_HOOK: name at 0x...`.
- Use `Config::enableOfflineApiMock` for behavior that should apply only in offline mode.

## Common lookups

| Need | Expression |
|---|---|
| Ordinary method | `Il2cppUtils::GetMethodPointer(assembly, namespace, class_name, method_name)` |
| Specific argument count | `Il2cppUtils::GetMethodIl2cpp(klass, "method_name", argument_count)` |
| Async state machine | `Il2cppUtils::find_nested_class(klass, predicate)`; match the `"<method_name>d__"` prefix and take `MoveNext` |
| Field offset | `Il2cppUtils::il2cpp_class_get_field_from_name(klass, "<>1__state")` |
| Internal call (icall) | `Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Application::get_version")` |

The async state-machine number (6 in `d__6`) may change between versions, so match by prefix. Class and method names can be searched in the iOS symbol table (`tools/re/il2cpp_re.py find`); the C# code is largely shared across platforms.

## Existing modules

| File | Contents |
|---|---|
| `hooks/HookShare.cpp` | API interception, response headers, asset version, destructive-action protection, Chapter RANK, push-token placeholder |
| `hooks/HookOfflineFixes.cpp` | Offline scene transitions, Home, mission lists, async exception handling, and diagnostic logs for login, gacha, and scenes |
| `hooks/HookSqlCipher.cpp` | Master database diagnostics (off by default; enable `traceMasterImport` or `dumpSqlCipherKey`) |
| `hooks/HookVirtualClock.cpp` | Virtual clock (`enableVirtualClock` + `virtualClockUnixTime`), off by default |

Some logging-only hooks in `HookOfflineFixes.cpp` are still installed unconditionally because they record state used by other hooks. Confirm each one before limiting it to debug builds.
