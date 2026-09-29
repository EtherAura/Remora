# device/remora — product registry (bd remora-28ix.4 R1).
#
# One product. arm64 and _only variants are deliberately absent: Remora ships
# remora_x86_64 and nothing else, and an unbuilt product definition is dead weight that
# drifts. If another arch is ever wanted, it gets added when someone
# commits to building and verifying it.

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/remora_x86_64.mk \

# A MENU, not a whitelist (bd remora-82c.12): cp2a is what Remora actually builds for
# Android 17 (SDK 37, codename REL), and cp1a lunches cleanly too — the release/ map
# carries the RELEASE_WEBAPP_MODULE override cp1a needs.
COMMON_LUNCH_CHOICES := \
    remora_x86_64-cp2a-userdebug \
    remora_x86_64-cp1a-userdebug \
