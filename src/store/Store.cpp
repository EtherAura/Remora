#include "core/Resolver.h"  // profileSlug — the container name derives from the profile's own name
#include "core/SourcePatches.h"
#include "store/Store.h"

#include <QSettings>
#include <QStandardPaths>

namespace remora {

QString defaultRemorarcPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/remorarc";
}

static QStringList splitList(const QString &s) {
    return s.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

// Input device names contain spaces ("Keychron K8 Pro Keyboard"), so the space-separated form the
// token lists above use cannot carry them: every multi-word name split back into one bogus entry
// per word, and the picker then showed those fragments as stale "not connected" rows. A newline is
// the one character an evdev name cannot hold — /proc/bus/input/devices prints one name per line —
// and QSettings escapes it as \n, so the value still occupies a single line in remorarc.
// A legacy space-joined value is deliberately NOT re-split here: the boundaries are unrecoverable
// from the string alone. It comes back as one entry, which the UI reconciles against the devices
// the host actually reports (see splitJoinedNames in MainWindow.cpp).
static QStringList splitNames(const QString &s) {
    return s.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QStringList listInstances(const QString &path) {
    QSettings s(path, QSettings::IniFormat);
    QStringList out;
    for (const QString &g : s.childGroups())
        if (g.startsWith(QLatin1String("Instance-"))) out << g.mid(9);
    out.sort();
    return out;
}

QString activeInstance(const QString &path, const QString &fallback) {
    QSettings s(path, QSettings::IniFormat);
    // NB: NOT "General/" — QSettings reserves the [General] section (escaped to [%General]), so a
    // literal [General] group written by hand or another tool is never read back. Use [Remora].
    return s.value(QStringLiteral("Remora/active_instance"), fallback).toString();
}

void setActiveInstance(const QString &path, const QString &name) {
    QSettings s(path, QSettings::IniFormat);
    s.setValue(QStringLiteral("Remora/active_instance"), name);
    s.sync();
}

RemoraConfig loadInstance(const QString &path, const QString &name) {
    QSettings s(path, QSettings::IniFormat);
    RemoraConfig c;
    const auto readGroup = [&](const QString &group) {
        s.beginGroup(group);
        const auto has = [&](const char *k) { return s.contains(k); };
        if (has("backend")) {
            const QString raw = s.value("backend").toString();
            c.backend.backend = backendFromString(raw);
            // Sticky across the layered read: [Defaults] then [Instance-x], and a profile that says
            // vm in either place is a vm profile.
            c.backend.retiredVmBackend = c.backend.retiredVmBackend || isRetiredVmBackend(raw);
        }
        if (has("ssh_host")) c.backend.sshHost = s.value("ssh_host").toString();
        if (has("container_name")) c.backend.containerName = s.value("container_name").toString();
        if (has("data_dir")) c.backend.dataDir = s.value("data_dir").toString();
        if (has("data_base_dir")) c.backend.dataBaseDir = s.value("data_base_dir").toString();
        if (has("host_adb_port")) c.backend.hostAdbPort = s.value("host_adb_port").toInt();
        if (has("width")) c.display.width = s.value("width").toInt();
        if (has("height")) c.display.height = s.value("height").toInt();
        if (has("dpi")) c.display.dpi = s.value("dpi").toInt();
        if (has("fps")) c.display.fps = s.value("fps").toInt();
        if (has("max_fps")) c.display.maxFps = s.value("max_fps").toInt();
        if (has("use_memfd")) c.display.useMemfd = s.value("use_memfd").toBool();
        if (has("timezone")) c.display.timezone = s.value("timezone").toString();
        // animator_scale / window_animation_scale / transition_animation_scale are RETIRED keys:
        // Remora no longer pins the animation durations, so Developer options owns them. An old
        // profile's values are read by nobody and rewritten by nobody.
        if (has("gpu_mode")) {
            // Unrecognised is kept aside for preflight to refuse, never defaulted. A later layer
            // that names a valid mode clears it: [Instance-x] overrides [Defaults] as usual.
            const QString raw = s.value("gpu_mode").toString().trimmed();
            if (const auto m = gpuModeFromString(raw)) {
                c.gpu.mode = *m;
                c.gpu.unrecognisedMode.reset();
            } else if (!raw.isEmpty()) {
                c.gpu.unrecognisedMode = raw;
            }
        }
        if (has("gpu_node")) c.gpu.gpuNode = s.value("gpu_node").toString();
        if (has("gpu_driver")) c.gpu.gpuDriver = s.value("gpu_driver").toString();
        if (has("gralloc")) c.gpu.gralloc = s.value("gralloc").toString();
        if (has("hwcomposer")) c.gpu.hwcomposer = s.value("hwcomposer").toString();
        if (has("vulkan")) c.gpu.vulkan = s.value("vulkan").toString();
        if (has("va_driver")) c.gpu.vaDriver = s.value("va_driver").toString();
        if (has("camera_devices"))
            c.input.cameraDevices = splitList(s.value("camera_devices").toString());
        if (has("cpufreq_topology")) c.input.cpufreqTopology = s.value("cpufreq_topology").toBool();
        if (has("venus")) c.gpu.venus = s.value("venus").toBool();
        if (has("venus_socket_dir")) c.gpu.venusSocketDir = s.value("venus_socket_dir").toString();
        if (has("venus_lib_dir")) c.gpu.venusLibDir = s.value("venus_lib_dir").toString();
        if (has("venus_server")) c.gpu.venusServerPath = s.value("venus_server").toString();
        if (has("image_tag")) c.image.imageTag = s.value("image_tag").toString();
        if (has("features")) c.image.features = splitList(s.value("features").toString());
        if (has("android_version")) c.image.androidVersion = s.value("android_version").toInt();
        if (has("build_host")) c.image.buildHost = s.value("build_host").toString();
        if (has("source_host")) c.image.sourceHost = s.value("source_host").toString();
        if (has("build_dir")) c.image.buildDir = s.value("build_dir").toString();
        if (has("source_tree")) c.image.sourceTree = s.value("source_tree").toString();
        if (has("source_kind")) c.image.sourceKind = s.value("source_kind").toString();
        if (has("source_pinned")) c.image.sourcePinned = s.value("source_pinned").toBool();
        if (has("build_nice")) c.image.buildNice = s.value("build_nice").toInt();
        if (has("build_jobs")) c.image.buildJobs = s.value("build_jobs").toInt();
        if (has("build_mem_gib")) c.image.buildMemGiB = s.value("build_mem_gib").toInt();
        if (has("soong_mem_gib")) c.image.soongMemGiB = s.value("soong_mem_gib").toInt();
        if (has("source_repo")) c.image.sourceRepo = s.value("source_repo").toString();
        if (has("source_ref")) c.image.sourceRef = s.value("source_ref").toString();
        if (has("built_commit")) c.image.builtCommit = s.value("built_commit").toString();
        if (has("built_recipe")) c.image.builtRecipe = s.value("built_recipe").toString();
        // Through the retirement map on the way in: source_patches is a PIN, so a key that has
        // been absorbed into another entry would otherwise silently drop that capability from
        // every profile saved before the consolidation (bd remora-82c.5).
        if (has("source_patches"))
            c.image.sourcePatches =
                canonicalPatchKeys(splitList(s.value("source_patches").toString()));
        if (has("arch_variant")) c.image.archVariant = s.value("arch_variant").toString();
        if (has("output_dir")) c.image.outputDir = s.value("output_dir").toString();
        if (has("network_mode")) c.network.networkMode = s.value("network_mode").toString();
        if (has("docker_network")) c.network.dockerNetwork = s.value("docker_network").toString();
        if (has("macvlan_ip")) c.network.macvlanIp = s.value("macvlan_ip").toString();
        if (has("macvlan_parent")) c.network.macvlanParent = s.value("macvlan_parent").toString();
        if (has("macvlan_subnet")) c.network.macvlanSubnet = s.value("macvlan_subnet").toString();
        if (has("macvlan_gateway"))
            c.network.macvlanGateway = s.value("macvlan_gateway").toString();
        if (has("macvlan_range")) c.network.macvlanRange = s.value("macvlan_range").toString();
        if (has("macvlan_host_route"))
            c.network.macvlanHostRoute = s.value("macvlan_host_route").toBool();
        if (has("macvlan_shim_ip")) c.network.macvlanShimIp = s.value("macvlan_shim_ip").toString();
        if (has("dns")) c.network.dns = splitList(s.value("dns").toString());
        if (has("proxy_type")) c.network.proxyType = s.value("proxy_type").toString();
        if (has("proxy_host")) c.network.proxyHost = s.value("proxy_host").toString();
        if (has("proxy_port")) c.network.proxyPort = s.value("proxy_port").toInt();
        // THE PRE-CUTOVER LEGACY-KEY READ-FALLBACKS ARE GONE (bd remora-28ix.4 step 4).
        // They loaded a legacy key when the new one was absent, and save() rewrote the profile to
        // the new spelling — so any profile written since the rename already carries the new names
        // and loses nothing. A profile old enough to have only the legacy key falls back to the
        // RESOLVER default for that field, which is the same behaviour as a profile that never set
        // it: nothing silently changes meaning, the setting simply reverts.
        // keyboard_mode / mouse_mode are RETIRED keys — sdk-style input is the only kind.
        if (has("mouse_bind")) c.input.mouseBind = s.value("mouse_bind").toString();
        if (has("shortcut_mod")) c.input.shortcutMod = s.value("shortcut_mod").toString();
        if (has("key_bind")) {
            // The value carries commas, QSettings' own INI list separator — a hand-written
            // unquoted key_bind=F1:b,F2:h arrives as a QStringList, so join it back.
            const QVariant v = s.value("key_bind");
            c.input.keyBind = v.userType() == QMetaType::QStringList
                                  ? v.toStringList().join(QLatin1Char(','))
                                  : v.toString();
        }
        if (has("pip_width")) c.mirror.pipWidth = s.value("pip_width").toInt();
        if (has("pip_height")) c.mirror.pipHeight = s.value("pip_height").toInt();
        if (has("kbd_identity")) c.input.kbdIdentity = s.value("kbd_identity").toString();
        if (has("shared_inputs")) c.input.sharedInputs = splitNames(s.value("shared_inputs").toString());
        if (has("video_bit_rate")) c.mirror.videoBitRate = s.value("video_bit_rate").toString();
        if (has("audio")) c.mirror.audio = s.value("audio").toBool();
        if (has("video_codec")) c.mirror.videoCodec = s.value("video_codec").toString();
        if (has("hw_decode")) c.mirror.hwDecode = s.value("hw_decode").toBool();
        if (has("host_encode")) c.mirror.hostEncode = s.value("host_encode").toBool();
        if (has("host_decode")) c.mirror.hostDecode = s.value("host_decode").toBool();
        // mirror_server_path / scrcpy_server_path are RETIRED (bd remora-28ix.5): the v1 jar
        // path is deleted; the device half is the agent the image bakes. Old values are read by
        // nobody and rewritten by nobody.
        if (has("host_encode_max_size"))
            c.mirror.hostEncodeMaxSize = s.value("host_encode_max_size").toInt();
        if (has("max_size")) c.mirror.maxSize = s.value("max_size").toInt();
        // audio_codec / audio_buffer are RETIRED keys — the mirror plays opus and nothing ever
        // emitted either. audio_gain is a RETIRED key — the quiet capture it compensated for is
        // fixed.
        if (has("fullscreen_separate_display")) c.mirror.fullscreenSeparateDisplay = s.value("fullscreen_separate_display").toBool();
        if (has("fullscreen_display")) c.mirror.fullscreenDisplay = s.value("fullscreen_display").toString();
        if (has("fullscreen_app")) c.mirror.fullscreenApp = s.value("fullscreen_app").toString();
        if (has("desktop_new_window")) c.mirror.desktopNewWindow = s.value("desktop_new_window").toBool();
        if (has("desktop_menu")) c.integration.desktopMenu = s.value("desktop_menu").toBool();
        if (has("app_modes")) c.integration.appModes = splitList(s.value("app_modes").toString());
        if (has("auto_update_apps")) c.integration.autoUpdateApps = s.value("auto_update_apps").toBool();
        if (has("auto_update_hours")) c.integration.autoUpdateHours = s.value("auto_update_hours").toInt();
        if (has("arm_apps")) c.integration.armApps = splitList(s.value("arm_apps").toString());
        if (has("arm_guard")) c.integration.armGuard = s.value("arm_guard").toBool();
        if (has("shared_folders"))
            c.integration.sharedFolders = splitList(s.value("shared_folders").toString());
        if (has("companion_apps"))
            c.integration.companionApps = splitList(s.value("companion_apps").toString());
        if (has("freeform_display")) c.mirror.freeformDisplay = s.value("freeform_display").toString();
        if (has("window_width")) c.mirror.windowWidth = s.value("window_width").toInt();
        if (has("window_height")) c.mirror.windowHeight = s.value("window_height").toInt();
        if (has("window_x")) c.mirror.windowX = s.value("window_x").toInt();
        if (has("window_y")) c.mirror.windowY = s.value("window_y").toInt();
        // window_borderless is a RETIRED key — the desktop owns the mirror's frame now.
        if (has("window_fullscreen"))
            c.mirror.windowFullscreen = s.value("window_fullscreen").toBool();
        if (has("mirror_extra")) c.mirror.extra = splitList(s.value("mirror_extra").toString());
        if (has("use_overlayfs")) c.advanced.useOverlayfs = s.value("use_overlayfs").toBool();
        // multi_audio_focus is a RETIRED key — independent audio is unconditional now.
        if (has("use_codec2")) c.advanced.useCodec2 = s.value("use_codec2").toBool();
        if (has("auto_sleep_minutes"))
            c.advanced.autoSleepMinutes = s.value("auto_sleep_minutes").toInt();
        if (has("ro_overrides")) c.advanced.roOverrides = splitList(s.value("ro_overrides").toString());
        if (has("root_hiding")) c.advanced.rootHiding = s.value("root_hiding").toString();
        if (has("play_spoof")) c.advanced.playSpoof = s.value("play_spoof").toBool();
        if (has("denylist_packages"))
            c.advanced.denylistPackages = splitList(s.value("denylist_packages").toString());
        s.endGroup();
    };
    readGroup(QStringLiteral("Defaults"));
    readGroup(QStringLiteral("Instance-") + name);
    // A profile that pins no container_name gets one DERIVED FROM ITS OWN NAME, the same
    // remora-<slug> allocateInstanceIdentity() hands a newly created profile. Done here rather
    // than in the resolver because the resolver never sees the profile name and so cannot tell two
    // profiles apart — its per-backend fallbacks are the generic "remora-bm"/"remora-remote",
    // which name no profile at all and collide the moment there is a second one.
    //
    // Deliberately does NOT override an existing pin. The container name is what Remora uses to
    // find a RUNNING instance, and several of its paths are derived from it and fixed at container
    // creation (the venus socket dir is bind-mounted from ~/.cache/remora/venus-<name>), so
    // changing the name of a live instance out from under it desyncs those until the next
    // recreate. An explicit name therefore stays exactly as written, and a rename is a deliberate
    // edit plus a `remora up`.
    // remora-<slug> since the identity rename (bd remora-28ix.4). For a hand-written section that
    // never pinned a name and already runs a pre-rename container, provision's
    // adoptLegacyContainer carries that container across the flip.
    if (!c.backend.containerName.has_value())
        c.backend.containerName = QStringLiteral("remora-%1").arg(profileSlug(name));
    return c;
}

void saveInstance(const QString &path, const QString &name, const RemoraConfig &c) {
    QSettings s(path, QSettings::IniFormat);
    // No group remove — write or remove KNOWN keys one by one, and only those. The old
    // remove-then-rewrite destroyed every key the WRITING binary's model lacked, and a stale
    // model is routine: the GUI saves on exit, and a GUI left running across an install keeps
    // pre-install code however new the binary on disk is. It cost a real failure — an old GUI,
    // exiting so its replacement could start, wiped the freshly hand-added audio= on its way out
    // (bd remora-9hvy). The price is that keys RETIRED from the model linger in remorarc
    // instead of evaporating on the next save; loaders already ignore what they don't read.
    // "Unset optional deletes the key" still holds — for keys the model knows.
    s.beginGroup(QStringLiteral("Instance-") + name);
    const auto wStr = [&](const char *k, const std::optional<QString> &v) {
        if (v) s.setValue(k, *v);
        else s.remove(k);
    };
    const auto wInt = [&](const char *k, const std::optional<int> &v) {
        if (v) s.setValue(k, *v);
        else s.remove(k);
    };
    const auto wBool = [&](const char *k, const std::optional<bool> &v) {
        if (v) s.setValue(k, *v);
        else s.remove(k);
    };
    const auto wList = [&](const char *k, const QStringList &v) {
        if (!v.isEmpty()) s.setValue(k, v.join(QLatin1Char(' ')));
        else s.remove(k);
    };
    if (c.backend.backend) s.setValue("backend", backendToString(*c.backend.backend));
    else s.remove("backend");
    wStr("ssh_host", c.backend.sshHost);
    wStr("container_name", c.backend.containerName);
    wStr("data_dir", c.backend.dataDir);
    wStr("data_base_dir", c.backend.dataBaseDir);
    wInt("host_adb_port", c.backend.hostAdbPort);
    wInt("width", c.display.width);
    wInt("height", c.display.height);
    wInt("dpi", c.display.dpi);
    wInt("fps", c.display.fps);
    wInt("max_fps", c.display.maxFps);
    wBool("use_memfd", c.display.useMemfd);
    wStr("timezone", c.display.timezone);
    if (c.gpu.mode) s.setValue("gpu_mode", gpuModeToString(*c.gpu.mode));
    else s.remove("gpu_mode");
    wStr("gpu_node", c.gpu.gpuNode);
    wStr("gpu_driver", c.gpu.gpuDriver);
    wStr("gralloc", c.gpu.gralloc);
    wStr("hwcomposer", c.gpu.hwcomposer);
    wStr("vulkan", c.gpu.vulkan);
    wStr("va_driver", c.gpu.vaDriver);
    // Not wList: an EXPLICITLY EMPTY list means "cameras off" and must persist as an empty value,
    // where wList's remove-on-empty would silently turn "off" back into "auto" (bd remora-4ei.9).
    if (c.input.cameraDevices.has_value())
        s.setValue("camera_devices", c.input.cameraDevices->join(QLatin1Char(' ')));
    else
        s.remove("camera_devices");
    wBool("cpufreq_topology", c.input.cpufreqTopology);
    wBool("venus", c.gpu.venus);
    wStr("venus_socket_dir", c.gpu.venusSocketDir);
    wStr("venus_lib_dir", c.gpu.venusLibDir);
    wStr("venus_server", c.gpu.venusServerPath);
    wStr("image_tag", c.image.imageTag);
    wList("features", c.image.features);
    wInt("android_version", c.image.androidVersion);
    wStr("build_host", c.image.buildHost);
    wStr("source_host", c.image.sourceHost);
    wStr("build_dir", c.image.buildDir);
    wStr("source_tree", c.image.sourceTree);
    wStr("source_kind", c.image.sourceKind);
    wBool("source_pinned", c.image.sourcePinned);
    wInt("build_nice", c.image.buildNice);
    wInt("build_jobs", c.image.buildJobs);
    wInt("build_mem_gib", c.image.buildMemGiB);
    wInt("soong_mem_gib", c.image.soongMemGiB);
    wStr("source_repo", c.image.sourceRepo);
    wStr("source_ref", c.image.sourceRef);
    wStr("built_commit", c.image.builtCommit);
    wStr("built_recipe", c.image.builtRecipe);
    wList("source_patches", c.image.sourcePatches);
    wStr("arch_variant", c.image.archVariant);
    wStr("output_dir", c.image.outputDir);
    wStr("network_mode", c.network.networkMode);
    wStr("docker_network", c.network.dockerNetwork);
    wStr("macvlan_ip", c.network.macvlanIp);
    wStr("macvlan_parent", c.network.macvlanParent);
    wStr("macvlan_subnet", c.network.macvlanSubnet);
    wStr("macvlan_gateway", c.network.macvlanGateway);
    wStr("macvlan_range", c.network.macvlanRange);
    wBool("macvlan_host_route", c.network.macvlanHostRoute);
    wStr("macvlan_shim_ip", c.network.macvlanShimIp);
    wList("dns", c.network.dns);
    wStr("proxy_type", c.network.proxyType);
    wStr("proxy_host", c.network.proxyHost);
    wInt("proxy_port", c.network.proxyPort);
    wInt("pip_width", c.mirror.pipWidth);
    wInt("pip_height", c.mirror.pipHeight);
    wStr("kbd_identity", c.input.kbdIdentity);
    // Not wList: device names contain spaces, so this key joins on newline instead (see
    // splitNames). Same write-or-remove contract as every other known key.
    if (!c.input.sharedInputs.isEmpty())
        s.setValue("shared_inputs", c.input.sharedInputs.join(QLatin1Char('\n')));
    else
        s.remove("shared_inputs");
    wStr("mouse_bind", c.input.mouseBind);
    wStr("shortcut_mod", c.input.shortcutMod);
    wStr("key_bind", c.input.keyBind);
    wStr("video_bit_rate", c.mirror.videoBitRate);
    wBool("audio", c.mirror.audio);
    wStr("video_codec", c.mirror.videoCodec);
    wBool("hw_decode", c.mirror.hwDecode);
    wBool("host_encode", c.mirror.hostEncode);
    wBool("host_decode", c.mirror.hostDecode);
    wInt("host_encode_max_size", c.mirror.hostEncodeMaxSize);
    wInt("max_size", c.mirror.maxSize);
    wBool("fullscreen_separate_display", c.mirror.fullscreenSeparateDisplay);
    wStr("fullscreen_display", c.mirror.fullscreenDisplay);
    wStr("fullscreen_app", c.mirror.fullscreenApp);
    wBool("desktop_new_window", c.mirror.desktopNewWindow);
    wBool("desktop_menu", c.integration.desktopMenu);
    wList("app_modes", c.integration.appModes);
    wBool("auto_update_apps", c.integration.autoUpdateApps);
    wInt("auto_update_hours", c.integration.autoUpdateHours);
    wList("arm_apps", c.integration.armApps);
    wBool("arm_guard", c.integration.armGuard);
    wList("shared_folders", c.integration.sharedFolders);
    wList("companion_apps", c.integration.companionApps);
    wStr("freeform_display", c.mirror.freeformDisplay);
    wInt("window_width", c.mirror.windowWidth);
    wInt("window_height", c.mirror.windowHeight);
    wInt("window_x", c.mirror.windowX);
    wInt("window_y", c.mirror.windowY);
    wBool("window_fullscreen", c.mirror.windowFullscreen);
    wList("mirror_extra", c.mirror.extra);
    wBool("use_overlayfs", c.advanced.useOverlayfs);
    wInt("auto_sleep_minutes", c.advanced.autoSleepMinutes);
    wBool("use_codec2", c.advanced.useCodec2);
    wList("ro_overrides", c.advanced.roOverrides);
    wStr("root_hiding", c.advanced.rootHiding);
    wBool("play_spoof", c.advanced.playSpoof);
    wList("denylist_packages", c.advanced.denylistPackages);
    s.endGroup();
    s.sync();
}


QList<Deployment> loadDeployments(const QString &path) {
    QSettings s(path, QSettings::IniFormat);
    QList<Deployment> out;
    for (const QString &g : s.childGroups()) {
        if (!g.startsWith(QLatin1String("Deployment-"))) continue;
        s.beginGroup(g);
        Deployment d;
        d.name = g.mid(11);
        d.profile = s.value(QStringLiteral("profile")).toString();
        d.host = s.value(QStringLiteral("host")).toString();
        d.image = s.value(QStringLiteral("image")).toString();
        s.endGroup();
        out << d;
    }
    return out;
}

void saveDeployments(const QString &path, const QList<Deployment> &deployments) {
    QSettings s(path, QSettings::IniFormat);
    // an explicitly saved list — even an empty one — must never be re-seeded
    s.setValue(QStringLiteral("Remora/deployments_initialized"), true);
    // Remove only groups whose deployment is GONE; rewriting kept groups whole would destroy
    // keys a newer binary added to them (bd remora-9hvy, same idiom as saveInstance).
    for (const QString &g : s.childGroups()) {
        if (!g.startsWith(QLatin1String("Deployment-"))) continue;
        const QString name = g.mid(11);
        bool kept = false;
        for (const Deployment &d : deployments)
            if (d.name == name) { kept = true; break; }
        if (!kept) s.remove(g);
    }
    for (const Deployment &d : deployments) {
        s.beginGroup(QStringLiteral("Deployment-") + d.name);
        s.setValue(QStringLiteral("profile"), d.profile);
        if (!d.host.isEmpty()) s.setValue(QStringLiteral("host"), d.host);
        else s.remove(QStringLiteral("host"));
        if (!d.image.isEmpty()) s.setValue(QStringLiteral("image"), d.image);
        else s.remove(QStringLiteral("image"));
        s.endGroup();
    }
    s.sync();
}

void deleteInstance(const QString &path, const QString &name) {
    QSettings s(path, QSettings::IniFormat);
    s.remove(QStringLiteral("Instance-") + name);
    s.sync();
}

QList<ImageSource> loadImageSources(const QString &path) {
    QSettings s(path, QSettings::IniFormat);
    QList<ImageSource> out;
    for (const QString &g : s.childGroups()) {
        if (!g.startsWith(QLatin1String("Source-"))) continue;
        const QString name = g.mid(7);
        s.beginGroup(g);
        ImageSource src;
        src.name = name;
        src.enabled = s.value("enabled", true).toBool();
        src.tags = s.value("tags").toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        src.location = s.value("location").toString().trimmed();
        s.endGroup();
        // A record with neither tags nor a location describes nothing. Real ones exist: the
        // retired built-ins (the upstream catalog, the erstt pair, Self-built — bd remora-2ylt)
        // persisted an enabled flag under their names, and those records would resurface here as
        // empty custom sources forever, since save rewrites what load returns. Dropped instead —
        // deleting a source stays possible, resurrecting one does not.
        if (src.tags.isEmpty() && src.location.isEmpty()) continue;
        out << src;
    }
    return out;
}

void saveImageSources(const QString &path, const QList<ImageSource> &sources) {
    QSettings s(path, QSettings::IniFormat);
    // Remove only groups whose source is GONE (deleting stays possible); kept groups get their
    // known keys written individually, so keys a newer binary added survive an older one's save
    // (bd remora-9hvy — location= was one save away from exactly that).
    for (const QString &g : s.childGroups()) {
        if (!g.startsWith(QLatin1String("Source-"))) continue;
        const QString name = g.mid(7);
        bool kept = false;
        for (const ImageSource &src : sources)
            if (src.name == name) { kept = true; break; }
        if (!kept) s.remove(g);
    }
    for (const ImageSource &src : sources) {
        s.beginGroup(QStringLiteral("Source-") + src.name);
        s.setValue("enabled", src.enabled);
        s.setValue("tags", src.tags.join(QLatin1Char(' ')));
        if (src.location.isEmpty())
            s.remove("location");
        else
            s.setValue("location", src.location);
        s.endGroup();
    }
    s.sync();
}

}  // namespace remora
