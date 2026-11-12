# Identify and check the inputs

## Choose a path

| Target | Ready-made inputs | Build inputs |
| --- | --- | --- |
| Android | Offline 5.1.0 APK and an Android resource pack directory, or its `.tar.zst` archive | User's 5.1.0 APK, this repository, and a cache snapshot plus matching private storage keys |
| iOS / iPadOS | Offline unsigned 5.0.1 IPA and a `container/` directory, or a private archive containing them | User's decrypted 5.0.1 IPA, this repository, and an iOS cache or a convertible Android cache |

The Android pack needs `files/D/` assets, the catalog and master databases in `files/M/`, and the matching `files/linkura-local/storage-key.json`. The iOS transfer needs `container/Documents/D/`, `container/Documents/M/`, and `container/Documents/LinkuraOffline/storage-key.json`. A resource archive does not replace the client APK or IPA. Source code is needed to rebuild clients or run the repository's installation tools; users of a ready-made APK and pack do not need to compile the Android module.

Confirm the actual file layout and versions; filenames and `MANIFEST.json` are hints, not proof. Check that the databases and storage key belong to the same pack. A cache without a master database cannot provide a working fresh-device installation. An incomplete cache may still contain missing game assets.

## Handle private archives

If a `.sha256` file is present, verify it before extraction:

```sh
shasum -a 256 -c path/to/archive.tar.zst.sha256
```

Run the checksum command from the directory containing the archive if the checksum file names only the archive's basename. Check the archive's member names and link targets before extracting it. Reject absolute paths, `..` traversal, or links that escape the staging directory. For `.tar.zst`, test decompression with `zstd -t path/to/archive.tar.zst`. Extract into a new private directory on a filesystem with enough free space for the expanded cache, build output, and transfer staging. Do not extract over a live cache or the original archive. A checksum verifies the downloaded archive against its companion checksum; it does not establish the provenance or completeness of the cache.

Android UnityFS bundles are not directly usable by the iOS client: they need platform retargeting and compatible shaders. Use the [resource conversion procedure](../../../../docs/en/resources.md#ios-from-android) when the only available cache is Android. Do not assume an iOS pack can be imported directly into Android.

Do not copy a source installation's `app` database or player identity into a newly shared pack. The current Android pack builder rekeys the catalog and master databases by default; `--keep-source-key` is an exceptional option that retains the source keys. Treat any existing private pack as sensitive even if its archive name is generic.
