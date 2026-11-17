# Experimental Magisk Zygisk for containers

`v30.7-container-zygisk.patch` applies to upstream Magisk tag `v30.7`, commit
`e8a58776f1d7bdf852072ad0baa6eceb9a1e4aac`. It is an opt-in source patch; the normal
Magisk download and published image do not apply it.

The patch contains the upstream Android 17 QPR2 JNI update from
[PR 9999](https://github.com/topjohnwu/Magisk/pull/9999), plus two container fixes:

- The `container-zygisk` Cargo feature accepts a Zygisk client without a peer security label
  only when the kernel reports that labels are unsupported, SELinux is absent, the peer is
  root, and its executable's device/inode match Android's `app_process32` or `app_process64`.
  Existing signature, client and root-policy checks remain enabled.
- Companion socket identity uses `fstat` before and after the module handler. The old
  `fd_get_attr` also requested `security.selinux`, which fails with `EOPNOTSUPP` on these
  sockets. That skipped the handler and left the requesting zygote child blocked.

Build from a separate checkout with its pinned submodules and upstream's ONDK:

```sh
git clone --recurse-submodules --branch v30.7 https://github.com/topjohnwu/Magisk.git
cd Magisk
git apply /path/to/Remora/vendor/source-build/features/magisk/patches/v30.7-container-zygisk.patch
cat > config.prop <<'EOF'
version=30.7-remora-container
versionCode=30700
abiList=x86_64
EOF
python3 build.py ndk
CARGO_BUILD_JOBS=3 python3 build.py -r native magisk
```

Set `ANDROID_HOME` to an Android SDK first. The patch limits build workers to three and enables
the feature for the `magisk` target while retaining its default security features. The output
is `native/out/x86_64/magisk`; retain the matching official v30.7 manager, stub, policy tool and
BusyBox. Use Remora's corrected `magisk-setup.sh` to build the boot environment.

In the private Android 17 test, unmodified Shamiko 0.7.5 (194) hid Magisk mounts from
a listed app with DenyList enforcement off. The same app saw the mounts when unlisted or with
Shamiko disabled, and lost them again after re-enabling it. Shamiko 1.2.5 (414) still reported
an unsupported environment. This establishes the tested hiding behavior, not universal root
detection resistance or a Play Integrity pass. Keep the module files unmodified: renaming
0.7.5's `service.sh` makes its integrity check reject the module. Its property script runs
alongside PIF's existing property pass; verify ADB and mirror startup with that configuration.

A private image whose modules are already installed and managed at boot may put the single
line `magisk` in `/system/etc/remora-spoof/zygisk-runtime`. Remora then verifies root, the native
companion executable and the database setting on reconnect. It leaves module installation and boot
settings to that image instead of running the ReZygisk installer. This marker does not install
or enable Zygisk on a fresh data directory. Configure the private image's modules and Magisk
database before selecting it, keep ReZygisk disabled, and retain a rollback image and profile.

The patch is part of the GPLv3 Magisk source modification; retain the source and build inputs
with any binary distribution. Shamiko and other proprietary payloads are owner supplied and
are not included here.
