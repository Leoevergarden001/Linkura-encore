package io.github.chocolzs.linkura.localify

import android.annotation.SuppressLint
import android.app.AndroidAppHelper
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Toast
import com.bytedance.shadowhook.ShadowHook
import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.IXposedHookZygoteInit
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedHelpers
import de.robv.android.xposed.callbacks.XC_LoadPackage
import org.json.JSONObject
import java.io.File
import java.util.zip.ZipFile

const val TAG = "LinkuraOffline"

/**
 * Xposed / LSPatch entry of the offline module.
 *
 * Runs inside the game process (com.oddno.lovelive):
 *  1. before Unity loads its native code: prepare the asset cache layout and
 *     seed the storage key of an imported resource pack into PlayerPrefs;
 *  2. after: start the native hooks (libHasuKikaisann), then hand them the
 *     built-in offline config and the client/resource version.
 *
 * The class name and package stay as in linkura-localify because the native
 * JNI symbols (Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_*)
 * and the LSPatch module id depend on them.
 */
class LinkuraHookMain : IXposedHookLoadPackage, IXposedHookZygoteInit {
    private lateinit var modulePath: String
    private val nativeLibLoaded: Boolean
    private var initialized = false

    override fun initZygote(startupParam: IXposedHookZygoteInit.StartupParam) {
        modulePath = startupParam.modulePath
    }

    override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
        if (lpparam.packageName != TARGET_PACKAGE) return

        val unityPlayer = lpparam.classLoader.loadClass("com.unity3d.player.UnityPlayer")
        XposedHelpers.findAndHookMethod(unityPlayer, "loadNative", String::class.java, object : XC_MethodHook() {
            override fun beforeHookedMethod(param: MethodHookParam) {
                // Unity has not read PlayerPrefs yet at this point.
                AndroidAppHelper.currentApplication()?.let {
                    prepareResourceDirs(it)
                    seedStorageKey(it)
                }
            }

            @SuppressLint("UnsafeDynamicallyLoadedCode")
            override fun afterHookedMethod(param: MethodHookParam) {
                if (initialized) return
                if (!nativeLibLoaded) {
                    showToast("Load native library lib$NATIVE_LIB.so failed.")
                    return
                }
                val app = AndroidAppHelper.currentApplication() ?: return
                val dataDir = File(app.filesDir, DATA_DIR).apply { mkdirs() }
                initHook("${app.applicationInfo.nativeLibraryDir}/libil2cpp.so", dataDir.absolutePath)
                initialized = true
                applyOfflineConfig(app, dataDir)
                startCallbackLoop()
            }
        })
    }

    /**
     * Config comes from <filesDir>/linkura-local/config/<name> when present
     * (handy with adb during development), otherwise from the defaults
     * bundled in this module (assets/offline-config/<name>).
     */
    private fun applyOfflineConfig(context: Context, dataDir: File) {
        val configDir = File(dataDir, "config")
        fun read(name: String, fallback: String): String {
            val override = File(configDir, name)
            if (override.isFile) {
                runCatching { return override.readText() }
            }
            return runCatching {
                ZipFile(modulePath).use { zip ->
                    val entry = zip.getEntry("assets/offline-config/$name") ?: return fallback
                    zip.getInputStream(entry).bufferedReader().use { it.readText() }
                }
            }.getOrElse {
                Log.e(TAG, "Built-in offline config $name unavailable", it)
                fallback
            }
        }

        val config = read("l4-config.json", "{}")
        loadConfig(config)
        loadArchiveConfig(read("archive-config.json", "[]"))

        val versionName = runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull().orEmpty()
        applyClientResVersion(read("client-res.json", "{}"), versionName, runCatching { JSONObject(config) }.getOrNull())
        Log.i(TAG, "Offline config applied (client $versionName)")
    }

    /**
     * client-res.json maps client versions (newest first) to their resource
     * versions (oldest first). The resource version decides which asset cache
     * the client accepts, so it must match the imported cache.
     * resourceVersionMode: 0 = the installed client's latest, 1 = newest
     * listed, 2 = customClientVersion / customResVersion.
     */
    private fun applyClientResVersion(clientResJson: String, versionName: String, config: JSONObject?) {
        val all = runCatching { JSONObject(clientResJson) }.getOrNull() ?: return
        fun lastOf(version: String): String =
            all.optJSONArray(version)?.let { if (it.length() > 0) it.getString(it.length() - 1) else "" } ?: ""

        val latestClient = all.keys().asSequence().firstOrNull() ?: versionName
        val latestRes = lastOf(latestClient)
        var targetClient = versionName
        var targetRes = lastOf(versionName)
        when (config?.optInt("resourceVersionMode", 0) ?: 0) {
            1 -> { targetClient = latestClient; targetRes = latestRes }
            2 -> {
                config?.optString("customClientVersion").takeUnless { it.isNullOrEmpty() }?.let { targetClient = it }
                config?.optString("customResVersion").takeUnless { it.isNullOrEmpty() }?.let { targetRes = it }
            }
        }
        if (targetRes.isEmpty() && latestRes.isEmpty()) {
            Log.w(TAG, "No resource version listed for client $versionName")
            return
        }
        Log.i(TAG, "Client/res: target $targetClient/$targetRes, latest $latestClient/$latestRes")
        loadClientResVersion(targetClient, targetRes, latestClient, latestRes)
    }

    // Resource-pack import without root. On Android 11+ `adb push` (shell
    // uid) may add files to the game's external dir but cannot create new
    // subdirectories there ("secure_mkdirs failed"). Create the cache layout
    // up front: D/ holds one directory per two-character base32 prefix of
    // the cached file names. Files pushed by adb belong to shell and are
    // read-only for the game; the M/ databases are re-created as the game's
    // own files so SQLite can open them read-write.
    private fun prepareResourceDirs(context: Context) {
        val ext = context.getExternalFilesDir(null) ?: return
        try {
            val alphabet = "abcdefghijklmnopqrstuvwxyz234567"
            val dRoot = File(ext, "D")
            var created = 0
            for (a in alphabet) for (b in alphabet) {
                val dir = File(dRoot, "$a$b")
                if (!dir.isDirectory && dir.mkdirs()) created++
            }
            val mRoot = File(ext, "M").apply { mkdirs() }
            File(ext, DATA_DIR).mkdirs()
            var adopted = 0
            mRoot.listFiles()?.forEach { file ->
                if (!file.isFile || file.canWrite() || file.name.endsWith(".adopt")) return@forEach
                val tmp = File(mRoot, file.name + ".adopt")
                file.copyTo(tmp, overwrite = true)
                if (file.delete() && tmp.renameTo(file)) adopted++ else tmp.delete()
            }
            if (created > 0 || adopted > 0) {
                Log.i(TAG, "Resource dirs prepared: $created D dirs created, $adopted M files adopted")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Preparing resource dirs failed", e)
        }
    }

    // The downloaded asset cache (external files D/ and M/) is named and
    // encrypted with a per-install local-storage key that the game keeps in
    // its encrypted PlayerPrefs. A fresh install generates a new key and
    // ignores an imported cache. A resource pack therefore ships the
    // matching PlayerPrefs entry in linkura-local/storage-key.json:
    //   {"prefs": "com.oddno.lovelive.v2.playerprefs",
    //    "entries": {"<xml name attribute>": "<xml value>"}}
    // It is looked up in the external files dir (next to D/ and M/), then in
    // the internal files dir. It is copied from a real install and is
    // account-bound, so it never belongs in a repository.
    private fun seedStorageKey(context: Context) {
        val candidates = listOfNotNull(
            context.getExternalFilesDir(null)?.let { File(it, "$DATA_DIR/storage-key.json") },
            File(context.filesDir, "$DATA_DIR/storage-key.json"),
        )
        val source = candidates.firstOrNull { it.isFile } ?: return
        try {
            val root = JSONObject(source.readText())
            val prefsName = root.optString("prefs", "${context.packageName}.v2.playerprefs")
            val entries = root.optJSONObject("entries") ?: return
            val prefs = context.getSharedPreferences(prefsName, Context.MODE_PRIVATE)
            val editor = prefs.edit()
            var changed = 0
            for (name in entries.keys()) {
                val value = entries.getString(name)
                if (prefs.getString(name, null) != value) {
                    editor.putString(name, value)
                    changed++
                }
            }
            if (changed > 0) editor.commit()
            Log.i(TAG, "Storage key seed from ${source.absolutePath}: ${entries.length()} entries, $changed updated")
        } catch (e: Exception) {
            Log.e(TAG, "Storage key seed failed (${source.absolutePath})", e)
        }
    }

    // Native code queues toasts; deliver them from a light background loop.
    private fun startCallbackLoop() {
        Thread({
            while (true) {
                runCatching { pluginCallbackLooper() }.onFailure { Log.e(TAG, "callback loop", it) }
                Thread.sleep(33)
            }
        }, "LinkuraOfflineLoop").apply { isDaemon = true }.start()
    }

    companion object {
        private const val TARGET_PACKAGE = "com.oddno.lovelive"
        private const val NATIVE_LIB = "HasuKikaisann"
        /** Module data directory in the game's files dir; the offline database sits next to it. */
        const val DATA_DIR = "linkura-local"

        @JvmStatic external fun initHook(targetLibraryPath: String, dataDir: String)
        @JvmStatic external fun loadConfig(configJsonStr: String)
        @JvmStatic external fun loadArchiveConfig(configJsonStr: String)
        @JvmStatic external fun loadClientResVersion(
            currentClientVersion: String, currentResVersion: String,
            latestClientVersion: String, latestResVersion: String,
        )
        @JvmStatic external fun pluginCallbackLooper(): Int
        /** Drop the offline player database (a fresh museum save on next start). */
        @JvmStatic external fun resetMockDatabase(filesDir: String): Boolean
        /** Rebuild the offline database from the built-in seed, keeping player state. */
        @JvmStatic external fun rebuildMockDatabase(filesDir: String): Boolean

        private var toast: Toast? = null

        /** Called from native code (Log::ShowToast). */
        @JvmStatic
        fun showToast(message: String) {
            val context = AndroidAppHelper.currentApplication()?.applicationContext
            if (context == null) {
                Log.e(TAG, "showToast: $message failed: applicationContext is null")
                return
            }
            Handler(Looper.getMainLooper()).post {
                toast?.cancel()
                toast = Toast.makeText(context, message, Toast.LENGTH_SHORT).also { it.show() }
            }
        }
    }

    init {
        ShadowHook.init(ShadowHook.ConfigBuilder().setMode(ShadowHook.Mode.UNIQUE).build())
        nativeLibLoaded = try {
            System.loadLibrary(NATIVE_LIB)
            true
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "System.loadLibrary($NATIVE_LIB) failed", e)
            false
        }
    }
}
