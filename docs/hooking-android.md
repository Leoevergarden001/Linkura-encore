# Android Hook

原生模块用 ShadowHook 做 inline hook，用 UnityResolve 和 IL2CPP API 按**名字**找类和方法，所以不依赖具体地址，客户端小版本变化时通常不用改。

## 写一个 Hook

```cpp
// hooks/HookShare.cpp 或 hooks/HookOfflineFixes.cpp
DEFINE_HOOK(int32_t, CommonApiCache_get_CurrentChapterRank, (void* self, void* mtd)) {
    const auto rank = CommonApiCache_get_CurrentChapterRank_Orig(self, mtd);
    return rank;
}

void Install(HookInstaller* hookInstaller) {
    ADD_HOOK(CommonApiCache_get_CurrentChapterRank, Il2cppUtils::GetMethodPointer(
            "Assembly-CSharp.dll", "Tecotec", "CommonApiCache", "get_CurrentChapterRank"));
}
```

- `DEFINE_HOOK(返回类型, 名字, (参数...))` 生成 `名字_Hook` 和原函数指针 `名字_Orig`。实例方法的第一个参数是 `self`，最后一个是 `MethodInfo*`。
- `ADD_HOOK(名字, 地址)` 安装，日志里会出现 `ADD_HOOK: 名字 at 0x...`。
- 需要离线时才生效的逻辑，用 `Config::enableOfflineApiMock` 判断。

## 常用查找

| 需求 | 写法 |
|---|---|
| 普通方法 | `Il2cppUtils::GetMethodPointer(程序集, 命名空间, 类名, 方法名)` |
| 指定参数个数 | `Il2cppUtils::GetMethodIl2cpp(klass, "方法名", 参数个数)` |
| async 状态机 | `Il2cppUtils::find_nested_class(klass, 判断函数)`，按名字前缀匹配 `"<方法名>d__"`，再取 `MoveNext` |
| 字段偏移 | `Il2cppUtils::il2cpp_class_get_field_from_name(klass, "<>1__state")` |
| 内部调用（icall） | `Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Application::get_version")` |

async 状态机的序号（`d__6` 里的 6）在不同版本间可能不同，用前缀匹配。类名和方法名可以在 iOS 符号表里查（`tools/re/il2cpp_re.py find`），两个平台的 C# 代码基本一致。

## 现有模块

| 文件 | 内容 |
|---|---|
| `hooks/HookShare.cpp` | 接管 API 请求、响应头、资源版本、危险操作保护、Chapter RANK、推送 token 占位 |
| `hooks/HookOfflineFixes.cpp` | 离线时的场景切换、主页、任务列表、异步异常处理；以及登录、扭蛋、场景的诊断日志 |
| `hooks/HookSqlCipher.cpp` | 主数据库访问的诊断（默认不输出，`traceMasterImport`、`dumpSqlCipherKey` 打开） |
| `hooks/HookVirtualClock.cpp` | 虚拟时钟（`enableVirtualClock` + `virtualClockUnixTime`），默认关闭 |

`HookOfflineFixes.cpp` 中只记日志的 Hook 目前仍然始终安装：有些会记录状态字段供其他 Hook 使用，逐个确认之前不要改成仅调试模式安装。
