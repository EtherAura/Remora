# The in-image Remora agent (bd remora-28ix.3.4): the device half of the mirror, baked into the
# image the way it will ship — replacing the bind-mounted /remora/agent.dex dev loop.
PRODUCT_PACKAGES += remora-agent remora-agent.rc
