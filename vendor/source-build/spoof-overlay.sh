# shellcheck shell=sh
# The Play spoof stack's staging: play_spoof's tree, Shamiko and the nb_sigfix module, as they sit
# under /system/etc/remora-spoof. Sourced by assemble-image.sh for a source build and by
# stage-spoof-layer.sh for the add-on layer (bd remora-6laf.3/.4), so both put the same tree there.
# Nothing runs when sourced. The deploy finds the stack by that directory alone (Deployers.cpp,
# playSpoof), so this tree IS the image half of play_spoof.

# stage_spoof_tree <zygisk_pif dir> <dest>: <dest> becomes remora-spoof without Shamiko or
# nb_sigfix: the ReZygisk module as installed, PlayIntegrityFork, the PIF profile, Remora's
# installer and the system-wide device spoof. <dest> must not exist yet: cp -a into an existing
# directory nests a copy inside it instead of replacing it.
stage_spoof_tree() {
  mkdir -p "$2" &&
    cp -a "$1/rez-mod" "$2/rez-mod" &&
    cp -a "$1/pifork" "$2/playintegrityfix" &&
    cp "$1/pif.json" "$2/pif.json" &&
    cp "$1/install.sh" "$2/install.sh" || return 1
  if [ -d "$1/devicespoof" ]; then cp -a "$1/devicespoof" "$2/devicespoof" || return 1; fi
}

# stage_spoof_shamiko <zygisk_pif dir> <dest>: <dest> becomes remora-spoof/shamiko, which
# install.sh installs as zygisk_shamiko when it is there.
stage_spoof_shamiko() {
  mkdir -p "$(dirname "$2")" && cp -a "$1/shamiko" "$2"
}

# stage_spoof_nb_sigfix <nb_sigfix dir> <dest>: <dest> becomes remora-spoof/nb_sigfix. Returns 1,
# staging nothing, when there is no built .so: the module is Remora's own and a public clone carries
# its source, not the binary.
stage_spoof_nb_sigfix() {
  [ -f "$1/zygisk/x86_64.so" ] || return 1
  mkdir -p "$2/zygisk" && cp "$1/module.prop" "$2/module.prop" &&
    cp "$1/zygisk/x86_64.so" "$2/zygisk/x86_64.so"
}
