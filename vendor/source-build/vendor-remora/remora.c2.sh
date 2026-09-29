#!/system/bin/sh
#
# Codec2 pipeline activation (bd remora-28ix.4 R2c).
#
# Only turn codec2 on when the kernel actually offers a DMA allocator the C2 buffer pool can
# use. Without one, ccodec=4 forces every app down a path whose allocations fail, which is a
# worse outcome than staying on the old codec path — so the test is the whole point of the
# script, and the reason this is a script rather than two setprop lines in the rc.
#
#   debug.stagefright.ccodec 4              force the codec2 (CCodec) path for all clients
#   debug.stagefright.c2inputsurface -1     use the C2 input surface (encoder input from a
#                                           Surface, which is how the mirror's HEVC encode
#                                           gets frames without a copy)
if [ -c /dev/dma_heap/system ] || [ -c /dev/ion ]; then
    setprop debug.stagefright.c2inputsurface -1
    setprop debug.stagefright.ccodec 4
fi
