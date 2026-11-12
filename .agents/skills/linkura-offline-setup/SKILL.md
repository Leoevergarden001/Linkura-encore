---
name: linkura-offline-setup
description: Prepare, build, install, or verify Linkura Preservation offline on Android or iOS using a user's own client and asset cache or a private prebuilt package. Use for device setup and resource transfer, not gameplay development.
---

# Linkura offline setup

Work from the root of the `linkura-offline` repository. Determine the target platform and whether the user has a ready-made package or source inputs. Read [input checks](references/inputs.md), then only the relevant [Android](references/android.md) or [iOS](references/ios.md) procedure. Use the repository's `tools/` for builds and transfers; consult the current `docs/en/` pages and CLI `--help` when a command or format may have changed.

Keep the user's original archives and caches intact. Stage extracted or converted files in a separate directory. Keep asset caches, storage keys, player databases, signing material, and account data outside Git; never print their contents or include them in a commit, issue, or response. Do not upload them to an external service.

Before changing a device, identify the exact target and inspect its existing installation and data. Do not wipe, clear data, uninstall, or overwrite player progress as a routine setup step. If a signature or provisioning conflict requires a data-losing action, explain the concrete conflict and preserve a backup before proceeding. A failed install or transfer is a reason to diagnose, not to reset the device.

Verify each completed stage. Report separately what was built, copied, checked on-device, and tested in the game. A successful file transfer alone does not prove that LOGIN, Home, or every asset works offline.
