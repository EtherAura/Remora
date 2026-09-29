# Remora in-Android system updater: a Settings-injected screen that shows
# whether a newer Remora-built image exists and lets the user apply it. "Apply" only signals —
# the HOST performs the actual update by recreating the container onto the new image (there is
# no recovery/update_engine in a container). Bridge = the shared /rezmods mount:
#   /rezmods/updater/state.json   host -> device: {"pending":bool,"tag","built","summary"}
#   /rezmods/updater/apply        device -> host: created by Apply; the host watcher acts on it
PRODUCT_PACKAGES += RemoraUpdater

# Replace the stock LineageOS updater (Settings > System > System updates) — our RemoraUpdater
# titles itself "System updates" and takes its place. PRODUCT_PACKAGES_REMOVE drops the module
# from a full build; the assemble step also deletes any already-built copy (belt-and-suspenders).
PRODUCT_PACKAGES_REMOVE += Updater
