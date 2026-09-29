#!/system/bin/sh

## enable `memfd` when `ashmem` is missing. init.rc disables memfd at post-fs-data, and a
## container may have neither node — system_server dies early without one of the two.
[ -c /dev/ashmem ] || setprop sys.use_memfd 1
