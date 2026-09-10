#include "../HookMain.h"

#ifndef GKMS_WINDOWS

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xdl.h"

namespace LinkuraLocal::HookSqlCipher {
    namespace {
        using SqliteHandle = void;

        std::mutex databasePathsMutex;
        std::unordered_map<SqliteHandle*, std::string> databasePaths;
        void* sqlCipherHandle = nullptr;
        void* systemSqliteHandle = nullptr;
        Il2cppUtils::FieldInfo* connectionDatabasePathField = nullptr;

        std::string CallerLocation(void* caller) {
            Dl_info info{};
            if (!caller || dladdr(caller, &info) == 0 || !info.dli_fname) {
                return "<unknown>";
            }
            const auto base = reinterpret_cast<uintptr_t>(info.dli_fbase);
            const auto address = reinterpret_cast<uintptr_t>(caller);
            return std::filesystem::path(info.dli_fname).filename().string()
                    + "+0x" + std::to_string(address - base);
        }

        std::string DatabasePath(SqliteHandle* db) {
            const std::scoped_lock lock(databasePathsMutex);
            const auto it = databasePaths.find(db);
            return it == databasePaths.end() ? "<unknown>" : it->second;
        }

        void RememberDatabasePath(SqliteHandle* db, const char* filename) {
            if (!db) return;
            const std::scoped_lock lock(databasePathsMutex);
            databasePaths[db] = filename ? filename : "<null>";
        }

        std::string HexEncode(const void* key, int keyBytes) {
            if (!key) return "<null>";

            size_t length = 0;
            if (keyBytes < 0) {
                length = std::strlen(static_cast<const char*>(key));
            } else {
                length = static_cast<size_t>(keyBytes);
            }

            // SQLCipher keys are normally short. Keep malformed inputs from
            // causing an unbounded read or log entry.
            constexpr size_t maxLoggedKeyBytes = 4096;
            length = std::min(length, maxLoggedKeyBytes);

            constexpr char digits[] = "0123456789abcdef";
            const auto* bytes = static_cast<const unsigned char*>(key);
            std::string result;
            result.resize(length * 2);
            for (size_t i = 0; i < length; ++i) {
                result[i * 2] = digits[bytes[i] >> 4];
                result[i * 2 + 1] = digits[bytes[i] & 0x0f];
            }
            return result;
        }

        void LogKey(SqliteHandle* db, const char* databaseName, const void* key, int keyBytes) {
            if (!Config::dumpSqlCipherKey) return;

            const auto path = DatabasePath(db);
            const auto keyHex = HexEncode(key, keyBytes);
            const auto actualLength = keyBytes < 0 && key
                    ? static_cast<int>(std::strlen(static_cast<const char*>(key)))
                    : keyBytes;
            Log::WarnFmt(
                    "[SqlCipherProbe] db=%p path=%s name=%s key_bytes=%d key_hex=%s",
                    db, path.c_str(), databaseName ? databaseName : "main",
                    actualLength, keyHex.c_str());
        }

        DEFINE_HOOK(int, sqlite3_open, (const char* filename, SqliteHandle** ppDb)) {
            const auto result = sqlite3_open_Orig(filename, ppDb);
            if (result == 0 && ppDb) RememberDatabasePath(*ppDb, filename);
            if (Config::dumpSqlCipherKey) {
                Log::InfoFmt("[SqlCipherProbe] sqlite3_open path=%s db=%p result=%d",
                             filename ? filename : "<null>", ppDb ? *ppDb : nullptr, result);
            }
            return result;
        }

        DEFINE_HOOK(int, sqlite3_open_v2,
                    (const char* filename, SqliteHandle** ppDb, int flags, const char* vfs)) {
            const auto result = sqlite3_open_v2_Orig(filename, ppDb, flags, vfs);
            if (result == 0 && ppDb) RememberDatabasePath(*ppDb, filename);
            if (Config::dumpSqlCipherKey) {
                Log::InfoFmt(
                        "[SqlCipherProbe] sqlite3_open_v2 path=%s db=%p flags=0x%x vfs=%s result=%d",
                        filename ? filename : "<null>", ppDb ? *ppDb : nullptr, flags,
                        vfs ? vfs : "<default>", result);
            }
            return result;
        }

        DEFINE_HOOK(int, sqlite3_key, (SqliteHandle* db, const void* key, int keyBytes)) {
            LogKey(db, "main", key, keyBytes);
            return sqlite3_key_Orig(db, key, keyBytes);
        }

        DEFINE_HOOK(int, sqlite3_key_v2,
                    (SqliteHandle* db, const char* databaseName, const void* key, int keyBytes)) {
            LogKey(db, databaseName, key, keyBytes);
            return sqlite3_key_v2_Orig(db, databaseName, key, keyBytes);
        }

        DEFINE_HOOK(int, system_sqlite3_open, (const char* filename, SqliteHandle** ppDb)) {
            const auto result = system_sqlite3_open_Orig(filename, ppDb);
            if (result == 0 && ppDb) RememberDatabasePath(*ppDb, filename);
            if (Config::dumpSqlCipherKey) {
                const auto caller = CallerLocation(__builtin_return_address(0));
                Log::InfoFmt(
                        "[SqliteProbe] provider=system api=sqlite3_open path=%s db=%p result=%d caller=%s",
                        filename ? filename : "<null>", ppDb ? *ppDb : nullptr, result,
                        caller.c_str());
            }
            return result;
        }

        DEFINE_HOOK(int, system_sqlite3_open_v2,
                    (const char* filename, SqliteHandle** ppDb, int flags, const char* vfs)) {
            const auto result = system_sqlite3_open_v2_Orig(filename, ppDb, flags, vfs);
            if (result == 0 && ppDb) RememberDatabasePath(*ppDb, filename);
            if (Config::dumpSqlCipherKey) {
                const auto caller = CallerLocation(__builtin_return_address(0));
                Log::InfoFmt(
                        "[SqliteProbe] provider=system api=sqlite3_open_v2 path=%s db=%p flags=0x%x vfs=%s result=%d caller=%s",
                        filename ? filename : "<null>", ppDb ? *ppDb : nullptr, flags,
                        vfs ? vfs : "<default>", result, caller.c_str());
            }
            return result;
        }

        DEFINE_HOOK(int, system_sqlite3_prepare_v2,
                    (SqliteHandle* db, const char* sql, int byteCount, void** statement,
                     const char** tail)) {
            if (Config::dumpSqlCipherKey && sql) {
                const std::string_view query(sql, byteCount < 0 ? std::strlen(sql)
                                                               : static_cast<size_t>(byteCount));
                if (query.find("LauncherBanners") != std::string_view::npos
                        || query.find("sqlite_master") != std::string_view::npos
                        || query.find("sqlite_schema") != std::string_view::npos
                        || query.find("PRAGMA") != std::string_view::npos
                        || query.find("CREATE TABLE") != std::string_view::npos) {
                    const auto caller = CallerLocation(__builtin_return_address(0));
                    const auto path = DatabasePath(db);
                    Log::InfoFmt(
                            "[SqliteProbe] provider=system api=sqlite3_prepare_v2 db=%p path=%s sql=%.*s caller=%s",
                            db, path.c_str(), static_cast<int>(query.size()), query.data(),
                            caller.c_str());
                }
            }
            return system_sqlite3_prepare_v2_Orig(db, sql, byteCount, statement, tail);
        }

        std::string ManagedConnectionPath(void* connection) {
            if (!connection || !connectionDatabasePathField) return "<unknown>";
            auto* value = Il2cppUtils::ClassGetFieldValue<Il2cppUtils::Il2CppString*>(
                    connection, connectionDatabasePathField);
            return value ? value->ToString() : "<null>";
        }

        void LogManagedKey(const char* source, void* connection,
                           const void* key, int keyBytes) {
            if (!Config::dumpSqlCipherKey) return;
            const auto keyHex = HexEncode(key, keyBytes);
            const auto path = ManagedConnectionPath(connection);
            Log::WarnFmt(
                    "[SqlCipherProbe] managed_source=%s connection=%p path=%s key_bytes=%d key_hex=%s",
                    source, connection, path.c_str(), keyBytes, keyHex.c_str());
        }

        bool IsMasterDatabaseConnection(void* connection) {
            constexpr std::string_view masterDirectory = "/files/M/";
            const auto path = ManagedConnectionPath(connection);
            return path.find(masterDirectory) != std::string::npos;
        }

        DEFINE_HOOK(void, SQLiteConnection_SetKeyString,
                    (void* self, Il2cppUtils::Il2CppString* key)) {
            if (key) {
                const auto value = key->ToString();
                LogManagedKey("SQLiteConnection.SetKey(System.String)", self, value.data(),
                              static_cast<int>(value.size()));
            } else {
                LogManagedKey("SQLiteConnection.SetKey(System.String)", self, nullptr, 0);
            }
            SQLiteConnection_SetKeyString_Orig(self, key);
        }

        DEFINE_HOOK(void, SQLiteConnection_SetKeyBytes,
                    (void* self, UnityResolve::UnityType::Array<uint8_t>* key)) {
            if (key) {
                LogManagedKey("SQLiteConnection.SetKey(System.Byte[])", self,
                              reinterpret_cast<void*>(key->GetData()),
                              static_cast<int>(key->max_length));
            } else {
                LogManagedKey("SQLiteConnection.SetKey(System.Byte[])", self, nullptr, 0);
            }
            SQLiteConnection_SetKeyBytes_Orig(self, key);
        }

        DEFINE_HOOK(void, SQLiteConnection_ConnectionStringCtor,
                    (void* self, void* connectionString)) {
            if (Config::dumpSqlCipherKey) {
                Log::InfoFmt("[SqlCipherProbe] SQLiteConnection(.ctor connectionString=%p)",
                             connectionString);
            }
            SQLiteConnection_ConnectionStringCtor_Orig(self, connectionString);
        }

        DEFINE_HOOK(void*, SQLiteConnection_CreateCommand,
                    (void* self, Il2cppUtils::Il2CppString* commandText,
                     UnityResolve::UnityType::Array<void*>* parameters)) {
            if (Config::dumpSqlCipherKey && commandText && IsMasterDatabaseConnection(self)) {
                const auto query = commandText->ToString();
                Log::InfoFmt(
                        "[MasterDataQuery] path=%s parameters=%zu sql=%s",
                        ManagedConnectionPath(self).c_str(),
                        parameters ? parameters->max_length : 0,
                        query.c_str());
            }
            return SQLiteConnection_CreateCommand_Orig(self, commandText, parameters);
        }

        bool MethodHasArguments(const UnityResolve::Method* method,
                                std::initializer_list<const char*> expectedTypes) {
            if (!method || method->args.size() != expectedTypes.size()) return false;
            auto expected = expectedTypes.begin();
            for (const auto* argument : method->args) {
                if (!argument || !argument->pType || argument->pType->name != *expected++) {
                    return false;
                }
            }
            return true;
        }

        void DumpManagedClass(UnityResolve::Class* klass, const char* label) {
            if (!Config::dumpSqlCipherKey || !klass) return;
            for (const auto* method : klass->methods) {
                if (!method) continue;
                std::string args = "(";
                for (size_t i = 0; i < method->args.size(); ++i) {
                    if (i) args += ",";
                    const auto* argument = method->args[i];
                    args += argument && argument->pType ? argument->pType->name : "?";
                }
                args += ")";
                Log::InfoFmt("[SqlCipherProbe] managed=%s method=%s args=%s return=%s ptr=%p",
                             label, method->name.c_str(), args.c_str(),
                             method->return_type ? method->return_type->name.c_str() : "?",
                             method->function);
            }
        }
    }

    void Install(HookInstaller* hookInstaller) {
        if (!hookInstaller || hookInstaller->m_il2cppLibraryPath.empty()) {
            Log::Warn("[SqlCipherProbe] missing hook installer or native library path");
            return;
        }

        const auto sqlCipherPath = std::filesystem::path(hookInstaller->m_il2cppLibraryPath)
                                           .parent_path() / "libsqlcipher.so";
        sqlCipherHandle = xdl_open(sqlCipherPath.c_str(), RTLD_NOW);
        if (!sqlCipherHandle) {
            Log::WarnFmt("[SqlCipherProbe] libsqlcipher unavailable at %s",
                         sqlCipherPath.c_str());
            return;
        }

        Log::InfoFmt("[SqlCipherProbe] installing hooks from %s", sqlCipherPath.c_str());
        ADD_HOOK(sqlite3_open, xdl_sym(sqlCipherHandle, "sqlite3_open", nullptr));
        ADD_HOOK(sqlite3_open_v2, xdl_sym(sqlCipherHandle, "sqlite3_open_v2", nullptr));
        ADD_HOOK(sqlite3_key, xdl_sym(sqlCipherHandle, "sqlite3_key", nullptr));
        ADD_HOOK(sqlite3_key_v2, xdl_sym(sqlCipherHandle, "sqlite3_key_v2", nullptr));

        systemSqliteHandle = xdl_open("/system/lib64/libsqlite.so", RTLD_NOW);
        if (!systemSqliteHandle) {
            Log::Warn("[SqliteProbe] system libsqlite unavailable");
            return;
        }
        Log::Info("[SqliteProbe] installing hooks from /system/lib64/libsqlite.so");
        ADD_HOOK(system_sqlite3_open,
                 xdl_sym(systemSqliteHandle, "sqlite3_open", nullptr));
        ADD_HOOK(system_sqlite3_open_v2,
                 xdl_sym(systemSqliteHandle, "sqlite3_open_v2", nullptr));
        ADD_HOOK(system_sqlite3_prepare_v2,
                 xdl_sym(systemSqliteHandle, "sqlite3_prepare_v2", nullptr));
    }

    void InstallManaged(HookInstaller* hookInstaller) {
        UnityResolve::Assembly* assembly = nullptr;
        UnityResolve::Class* connection = nullptr;
        for (auto* candidate : UnityResolve::assembly) {
            if (!candidate) continue;
            connection = candidate->Get("SQLiteConnection", "SQLite");
            if (connection) {
                assembly = candidate;
                break;
            }
        }
        if (!assembly || !connection) {
            Log::Warn("[SqlCipherProbe] SQLite.SQLiteConnection unavailable");
            return;
        }
        Log::InfoFmt("[SqlCipherProbe] managed SQLite assembly=%s",
                     assembly->name.c_str());
        connectionDatabasePathField = Il2cppUtils::il2cpp_class_get_field_from_name(
                connection->address, "<DatabasePath>k__BackingField");
        if (!connectionDatabasePathField) {
            Log::Warn("[SqlCipherProbe] SQLiteConnection.DatabasePath backing field unavailable");
        }
        auto* connectionString = assembly->Get("SQLiteConnectionString", "SQLite");
        auto* sqlite3 = assembly->Get("SQLite3", "SQLite");
        DumpManagedClass(connection, "SQLite.SQLiteConnection");
        DumpManagedClass(connectionString, "SQLite.SQLiteConnectionString");
        DumpManagedClass(sqlite3, "SQLite.SQLite3");

        for (const auto* method : connection->methods) {
            if (!method || !method->function) continue;
            if (method->name == "SetKey"
                    && MethodHasArguments(method, {"System.String"})) {
                ADD_HOOK(SQLiteConnection_SetKeyString, method->function);
            } else if (method->name == "SetKey"
                    && MethodHasArguments(method, {"System.Byte[]"})) {
                ADD_HOOK(SQLiteConnection_SetKeyBytes, method->function);
            } else if (method->name == ".ctor"
                    && MethodHasArguments(method, {"SQLite.SQLiteConnectionString"})) {
                ADD_HOOK(SQLiteConnection_ConnectionStringCtor, method->function);
            } else if (method->name == "CreateCommand"
                    && MethodHasArguments(method, {"System.String", "System.Object[]"})) {
                ADD_HOOK(SQLiteConnection_CreateCommand, method->function);
            }
        }
    }
}

#else

namespace LinkuraLocal::HookSqlCipher {
    void Install(HookInstaller*) {}
    void InstallManaged(HookInstaller*) {}
}

#endif
