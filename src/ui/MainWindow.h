#pragma once
#include <QColor>
#include <QMainWindow>
#include <QMap>
#include <QSet>

#include <QHash>

#include <atomic>

#include "core/Backend.h"
#include "core/Config.h"
#include "core/Resolver.h"
#include "store/Store.h"

class QComboBox;
class QFileSystemWatcher;
class QKeySequenceEdit;
class QPushButton;
class QPlainTextEdit;
class QSpinBox;
class QToolButton;
class QLineEdit;
class QCheckBox;
class QLabel;
class QTreeWidget;
class QAction;
class QListWidget;
class QStackedWidget;
class QVariantAnimation;
class QVBoxLayout;
class QGridLayout;
class QFormLayout;
class QTreeWidgetItem;
class QTimer;

namespace remora {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    // Jump to a page by its sidebar title (REMORA_SHOT_PAGE screenshots); false if unknown.
    bool selectPage(const QString &title);
    void warnIfPageOverflows() const;  // REMORA_SHOT: report a page wider than its viewport
    // Show/hide the log pane (REMORA_SHOT_LOG screenshots — same toggle as the status bar's).
    void setLogVisible(bool visible);
    // Collapse/expand the sidebar (the sidebar's own chevron + REMORA_SHOT_COLLAPSED shots).
    void setSidebarCollapsed(bool collapsed);

protected:
    // Terminate an in-flight source pull when the window closes, so it doesn't orphan to init.
    void closeEvent(class QCloseEvent *event) override;
    // Re-derive the accent styling when the desktop palette (wallpaper accent) changes.
    void changeEvent(QEvent *event) override;

private slots:
    void onAddSource();
    void onRemoveSource();
    void onSourceToggled();
    void onRefreshArchives();
    void onRenameArchive();
    void onDeleteArchive();
    void onPromoteImage();
    void onActiveImageChanged();
    void onPushImage();
    void onFetchSourceImage();
    void onPruneImages();
    void onCleanBuildDir();
    // measure → ask → delete, each off the GUI thread; the Clean buttons' shared body
    void reclaimDir(const QString &host, const QString &dir, const QString &title,
                    const QString &question, const QString &emptyMsg);
    void onCleanSourceOutput();
    void onPullSource();
    void onBuildFromSource();
    void onRefreshSourceRefs();
    void populateSourceRefCombo();  // filter fetched refs to the selected Android version
    QString stageSourceAssets(const QString &host);
    void onMarkBuilt();
    void switchProfile(const QString &name);  // save the current profile, load the named one
    void onNewProfile();
    void onLoadProfile();
    void onRenameProfile();
    void onDeleteProfile();
    void onRemoveAppMenu();   // Integration card: remove the selected profile's menu folder
    // Fetch+sideload an app's ARM64 build into a profile's container (transfer dialog's op).
    void installArmApp(const QString &profile, const QString &pkg);
    // Update every third-party app in place from Play, ABI-preserving (bar button + auto-update).
    void runAppUpdate(const QString &profile, bool manual);
    void maybeAutoUpdateApps(const QString &profile);  // after Connect, if opted in and due
    // After Connect: re-fetch any arm64-pinned app Play has overwritten with an x86_64 build.
    void runArmGuard(const QString &profile);
    void onTransferApps();    // Apps page: transfer/export/import installed APKs dialog
    void onOpenDeviceStorage();  // Apps page: browse <dataDir>/media/0 (= the device's /sdcard)
    // Shown when that path is not ours to open: the POSIX-ACL grant on this profile's storage,
    // behind pkexec. `pending` is reopened once the grant lands, so one click still gets there.
    void offerDeviceStorageAccess(const QString &media, const QUrl &pending);
    void autosave();  // persist every config change immediately (no Save button)
    void recompute();
    void onState(QString step, QString state, QString detail, QString stderrTail);
    void onDone(bool ok);
    void onConflict(QString holder);

private:
    // Surface a failed step as a dialog. The checklist already reddens the row, but a long chain
    // scrolls it out of view and the log pane never opens itself, so a failure part-way into a
    // multi-minute build was easy to walk away from and miss.
    void showFailure(const QString &step, const QString &detail, const QString &stderrTail);
    bool failureDialogOpen_ = false;  // guard: one dialog, not a stack of them per run
    void buildUi();
    void loadConfig();
    void wireAutosave();          // connect every config widget's change signal to autosave()
    bool suppressAutosave_ = false;  // guard while programmatically loading widgets
    Backend currentBackend() const;
    void applyToConfig();
    void setRunning(bool running);

    QLineEdit *dataDirEdit_ = nullptr;
    // The other two thirds of the per-profile identity (bd remora-4u4.8). Pinned at creation
    // alongside dataDir, so leaving them invisible meant a user could not see which port their
    // instance was on — which is precisely what you need when several are running.
    QLineEdit *hostAdbPortEdit_ = nullptr;
    QLabel *buildDirSizeLabel_ = nullptr,
           *sourceOutSizeLabel_ = nullptr, *prunableLabel_ = nullptr;
    // The profile's Android release: creation-time state, no widget — see androidVersion().
    int androidVersion() const;
    QMap<QString, QCheckBox *> featureChecks_;
    QMap<QString, QCheckBox *> patchChecks_;
    // The widget actually added to the layout for each patch — usually the checkbox, but a few
    // patches sit in a row with a companion control, and hiding the bare checkbox would leave that
    // control orphaned on screen (bd remora-82c.3).
    QMap<QString, QWidget *> patchRows_;
    // The golden stack has NO widgets: it is carried verbatim from remorarc to remorarc so the
    // page cannot silently drop a patch it stopped rendering. patchChecks_/patchRows_ above hold
    // the optional tier only; this holds the rest, and applyToConfig writes the union.
    QStringList goldenSelection_;
    QMap<QString, QCheckBox *> goldenChecks_;  // read-only mirror of the above
    QToolButton *goldenDisclosure_ = nullptr;  // titled with the applied count
    void refreshBaselineTitle();
    // Every patch this page currently has ON, across BOTH tiers. The ONLY correct way to ask:
    // patchChecks_ is the optional tier alone, so a caller that walks it directly sees an empty
    // golden stack. That is not a cosmetic difference — onBuildFromSource asked it directly and
    // handed the result straight to SourceBuildPlan::enabledPatches, so a profile with all
    // twenty-odd golden patches stored was warned they were unticked and would then have BUILT
    // without them (bd remora-28ix.3.4).
    QStringList selectedPatchKeys() const;
    QMap<QString, QLabel *> sectionHeaders_;  // one per featureSections() entry
    // Is the NVIDIA/Venus path selected right now? Read from the widgets rather than cfg_, which
    // a refresh triggered by a combo change has not been written back to yet.
    bool venusSelected() const;
    // Show only the patches that have something to apply on the selected Android version. A
    // version-filtered view, refreshed on version change — the same shape as
    // populateSourceRefCombo.
    void refreshPatchApplicability();

    // The optional patch tier is a 2-col grid whose cells are re-packed whenever the
    // visible set changes, so filtering never leaves holes in it.
    // One grid for the whole list: features lead, visible optional patches follow.
    QGridLayout *featureGrid_ = nullptr;
    QStringList featureOrder_;  // registry order, so repacking is stable
    void repackFeatureGrid();
    // Feature tooltips have two independent contributors (see the definition) — each writes its
    // half here and calls applyFeatureTooltips(), which is the only thing that calls setToolTip.
    QMap<QString, QString> featureGateReason_;    // from recompute() / the gating engine
    QMap<QString, QString> featureInapplicable_;  // from refreshPatchApplicability() / patch assets
    void applyFeatureTooltips();
    QLineEdit *widthEdit_, *heightEdit_, *dpiEdit_, *macvlanEdit_;
    // macvlan network shape — all blank by default (the deploy derives them from the docker host)
    QLineEdit *macvlanSubnetEdit_ = nullptr, *macvlanGatewayEdit_ = nullptr,
              *macvlanParentEdit_ = nullptr, *macvlanRangeEdit_ = nullptr;
    QComboBox *fpsCombo_ = nullptr;  // display refresh rate (Hz); editable, blank = default
    QLineEdit *bitrateEdit_, *dnsEdit_, *roEdit_;
    QComboBox *proxyTypeCombo_ = nullptr;  // Device > Network proxy (bd remora-w02j)
    QLineEdit *proxyHostEdit_ = nullptr, *proxyPortEdit_ = nullptr;
    // Navigation card: one combo per Android action (Back/Home/Recents/Notifications, in
    // kNavActions order) naming the mouse button that triggers it, with the keyboard chord for the
    // same action beside it. Together they edit the mirror's --mouse-bind vector and --shortcut-mod,
    // neither of which is ever typed here: both are positional and a typo disables a button in
    // silence.
    QList<QComboBox *> navActionCombos_;
    QList<QLabel *> navKeyHints_;
    QComboBox *shortcutModCombo_ = nullptr;
    // The mouse_bind this profile arrived with. Slots no action claims are written back from it,
    // so a hand-written '-' (ignore the click) survives a save from a card that has no row for it.
    QString navBindRaw_;
    // Direct key bindings (--key-bind): one capture field per action, kNavActions order.
    // navKeyRawNames_ holds, per row, a hand-written SDL key name the card could not translate
    // into a Qt key — it is re-emitted verbatim on save until the row is edited, so opening the
    // page cannot quietly unbind a key it has no picture of.
    QList<QKeySequenceEdit *> navKeyEdits_;
    QStringList navKeyRawNames_;
    void syncNavKeyHints();  // re-label the per-action chords after a modifier change
    QLineEdit *winWidthEdit_ = nullptr, *winHeightEdit_ = nullptr;
    QLineEdit *fsDisplayEdit_ = nullptr, *fsAppEdit_ = nullptr;
    QCheckBox *fsSeparateCheck_ = nullptr, *fsNewWindowCheck_ = nullptr;
    QLineEdit *autoSleepEdit_ = nullptr;  // idle minutes before auto-freeze (blank = off)
    QLineEdit *outputDirEdit_ = nullptr, *sourceTreeEdit_ = nullptr;
    QComboBox *sourceKindCombo_ = nullptr;
    QComboBox *sourceRefCombo_ = nullptr;
    // Settings page: one row per profile — its desktop-menu / app-update / ARM-guard switches
    // and Play-certification verdict as columns, Apps-page style (per-row widgets, flat bar).
    QTreeWidget *profilesTree_ = nullptr;
    // Certification verdicts by profile name (bd remora-4ei.83). Cached OUTSIDE the tree because
    // the tree rebuilds on every add/rename/switch, and a rebuild must not wipe a verdict that
    // cost a device round-trip; a rename migrates the key, a delete drops it.
    struct ProfileCertState {
        QString cell, tip, androidId;
        QColor color;
    };
    QHash<QString, ProfileCertState> profileCert_;
    void applyCertCell(QTreeWidgetItem *item);
    QTreeWidgetItem *profileItemFor(const QString &name) const;
    // The certification bar's target: the selected profiles row, else the active profile.
    QString selectedProfileName() const;
    // One terse token in the status bar: "source ✓" / "source: not built · ↓3".
    QLabel *sourceStateLabel_ = nullptr;
    void refreshSourceState();
    QTreeWidget *runTree_ = nullptr;
    // recheck=true forces the GMS checkin first, which is what makes a just-registered
    // android_id take effect. restore=true writes the snapshotted checkin identity back FIRST
    // (bd remora-4ei.84), then reads the state it produced. Both flags off is a plain read —
    // which still saves a snapshot. Acts on selectedProfileName()'s row.
    void refreshCertification(bool recheck, bool restore = false);
    QLineEdit *sharedFoldersEdit_ = nullptr;  // Apps page: "<hostDir>[:<name>]" shares list
    enum RunAction { RunStart, RunConnect, RunStop, RunDesktop, RunSleep };
    QString lastRefRepo_;
    QStringList sourceRefsAll_;  // unfiltered ls-remote refs; the combo shows a version-filtered view
    QComboBox *rootHidingCombo_ = nullptr, *videoCodecCombo_ = nullptr;
    QComboBox *gpuDriverCombo_ = nullptr;  // Device page: multi-GPU pick by driver (bd remora-e5x.1)
    QComboBox *venusCombo_ = nullptr;  // NVIDIA Venus: auto / on / off (bd remora-4ei.56)
    // Shown only for a profile whose Venus state the GPU list cannot express (off, no GPU).
    QLabel *venusRowLabel_ = nullptr;
    QComboBox *networkModeCombo_ = nullptr;
    QCheckBox *audioCheck_ = nullptr;  // mirror audio (audio= in remorarc, --audio)
    QTreeWidget *integrationsTree_ = nullptr;  // the active profile's app entries + window modes
    void rebuildIntegrationsList();
    // Root-hiding denylist as the table sees it: the on-disk list, re-read per rebuild.
    QStringList denylistPkgs_;
    QLabel *denylistOrphanLabel_ = nullptr;  // denylisted pkgs with no launcher entry
    void setEntryDenylisted(const QString &pkg, bool on);
    // Apps page: auto-refresh whenever the page is opened (replaces the Refresh-now button).
    int appsPageIndex_ = -1;
    // Runners page: visit-triggered refresh + in-place status polling while the page is open
    int runnersPageIndex_ = -1;
    int devicePageIndex_ = -1;  // visit re-probes whether the data dir may be edited
    QTimer *runnersPoll_ = nullptr;
    void probeRunStatuses();  // async docker-ps sweep, updates chips without rebuilding rows
    void setRunPill(QTreeWidgetItem *it, const QString &text, const QString &kind,
                    const QString &tooltip = {});
    void markDeploymentsBusy(const QString &profile, const QString &verb);
    bool appsRefreshInFlight_ = false;
    void refreshAppsPage();
    // Persist one entry's window mode into the active profile's remorarc section.
    void setEntryMode(const QString &pkg, const QString &mode);
    void probeArmApps();            // async: which apps run as arm64 (badges the list)
    QSet<QString> armApps_;         // last probe's answer, keyed by package
    // Each arm64 app's updateOwnerPackageName from the same probe (empty = no claim recorded).
    // The owner is what separates "protected" (shell claim — Play cannot touch it) from
    // "repaired after the fact" (Play-owned/unclaimed — bd remora-4ei.91), so the badge needs it.
    QHash<QString, QString> armOwners_;
    bool armProbeInFlight_ = false;
    // Clear a profile's menu folder before its name stops resolving (bd remora-4ei.77).
    void removeAppMenuFolder(const QString &profile);
    void setAllAppModes(bool desktop);
    void openRedditLogin(const QString &profileName);  // embedded reddit.com login for a profile
    QSpinBox *buildNiceSpin_ = nullptr, *buildJobsSpin_ = nullptr, *buildMemSpin_ = nullptr;
    QComboBox *archVariantCombo_ = nullptr;  // Source build: TARGET_ARCH_VARIANT (blank = portable)
    QSpinBox *soongMemSpin_ = nullptr;  // paired with the soong_gomemlimit patch checkbox
    QTreeWidget *checklist_ = nullptr, *sourcesTree_ = nullptr, *archivesTree_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;        // terminal-style command/step output
    QWidget *logPane_ = nullptr;               // bottom pane: checklist + logView (status-bar toggle)
    QAction *logAction_ = nullptr;
    void appendLog(const QString &text);
    void applyAccentStyle();
    // status-bar busy indicator: a timer-driven glyph spinner (QProgressBar's indeterminate
    // animation freezes under an app stylesheet)
    QLabel *busySpinner_ = nullptr;
    QTimer *spinnerTimer_ = nullptr;
    int spinnerFrame_ = 0;
    QList<QPushButton *> sourceButtons_;  // Pull/Apply/Build/Mark/Clean — gated while busy
    QList<QPushButton *> runButtons_;     // Run-row Start/Connect/Stop — gated while busy
                                           // skips its own confirm (avoids a double dialog)
    QComboBox *activeImageCombo_ = nullptr;  // which tag this profile deploys — and the pin itself
    QString derivedImageTag() const;
    // Re-apply everything that depends on which image this profile deploys.
    void refreshImageGating();
    qint64 sourceOutSizeBytes_ = -1;  // <source tree>/out as the last library scan measured it (-1 = unknown)
    int archiveScanGen_ = 0;  // bumped per library scan; a stage from an older scan drops itself
    std::atomic<qint64> pullChildPid_{0};  // process-group leader of a running pull (0 = none)
    QList<ImageSource> sources_;  // user-configured image sources (no built-ins — bd remora-2ylt)
    void rebuildSourcesTree();
    void rebuildProfilesList();
    void rebuildRunTable();
    void onAddDeployment();
    void onEditDeployment();
    void onRemoveDeployment();
    bool deploymentDialog(Deployment &d, bool isNew);
    void runProfile(const QString &name, const QString &hostOverride, int action,
                    const QString &imageOverride = QString());
    // Per-deployment screenshot (deliberately not gated on running_ — a read-only adb capture).
    void shotProfile(const QString &profile, const QString &hostOverride);
    // Per-deployment record toggle: relaunches that deployment's mirror with/without --record.
    void recordProfile(const QString &depName, const QString &profile,
                       const QString &hostOverride, const QString &imageOverride, bool on);
    bool gateVideoCodec(RemoraConfig &cfg, Backend backend);  // false = user cancelled
    void syncProfileWidgets();
    QLineEdit *addBrowseDir(class QFormLayout *form, const QString &label, const QString &placeholder);
    QString archivesDir() const;
    QString buildDirPath() const;
    QString sourceRepoStr() const;  // remorarc source_repo, else derived from the kind
    QString sourceTreePath() const;
    QString buildHostStr() const;
    QString sourceHostStr() const;
    QString recordPath_;      // the mp4 the current/last recording writes to
    QString recordingDep_;    // deployment name whose mirror is recording (empty = none)

    // sidebar navigation (Kartend-style: icon list + grouped stacked pages)
    QListWidget *sidebar_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QStringList sidebarLabels_;
    QWidget *sidebarPane_ = nullptr;   // profile entry + nav list + collapse toggle
    QVariantAnimation *sidebarAnim_ = nullptr;  // the collapse/expand width slide
    // The profile entry: icon (always visible; opens the profile menu) + the active profile's
    // name + switch/add buttons (the labelled parts hide at the 56px collapsed width).
    QWidget *profileRow_ = nullptr;
    QLabel *profileNameLabel_ = nullptr;
    QToolButton *profileSwitchBtn_ = nullptr, *profileAddBtn_ = nullptr;
    void showProfileMenu();  // pick a profile or create one — the icon's click target
    QVBoxLayout *newPage(const QString &title, std::initializer_list<const char *> icons);

    // conditional rows: hidden when inapplicable (backend/network mode), values preserved
    QFormLayout *deviceForm_ = nullptr;
    QWidget *dataDirRow_ = nullptr;
    // One split button on the Data dir row: the primary click opens the device's /sdcard, the
    // drop-down also offers repointing the dir. Both actions are always visible, so neither is
    // something you have to discover — but the picker greys out while the device is running.
    QToolButton *dataDirBtn_ = nullptr;
    QAction *dataDirPickAct_ = nullptr;
    // The data dir is what the running container is BOUND to, so it may only be repointed while
    // the device is down. Asked of docker rather than inferred, and re-asked on the events that
    // can change the answer (profile switch, Device-page visit, the end of any run action).
    void refreshDataDirLock();
    void setDataDirLocked(bool locked);
    void syncConditionalRows();

    QComboBox *kbdIdentityCombo_ = nullptr;
    void populateKbdIdentities();
    QListWidget *sharedInputsList_ = nullptr;
    void populateSharedInputs();

    QString path_, instance_;
    RemoraConfig cfg_;

    // remorarc divergence guard (bd remora-4ei.18). A running GUI used to hold remorarc in memory
    // and write it back wholesale, so any edit made to the file meanwhile — by hand, by the CLI,
    // by another session — was invisible here and silently clobbered by the next autosave. The
    // fingerprint is what lets a watcher event tell OUR write from someone else's.
    QFileSystemWatcher *rcWatcher_ = nullptr;
    QByteArray rcFingerprint_;
    bool rcCheckPending_ = false;
    QByteArray remorarcFingerprint() const;
    void noteRemorarcWritten();
    void watchRemorarc();
    void onRemorarcChanged();
    void reloadRemorarc();
    QSet<QString> features_;
    bool running_ = false;
    bool archiveScanBusy_ = false;
};

}  // namespace remora
