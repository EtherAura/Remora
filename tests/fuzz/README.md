# Fuzz targets

libFuzzer harnesses for the mirror protocol's parsers of guest-controlled bytes. Off by default,
Clang only, and built in a directory of their own; the ordinary GCC build and its test run never
see them.

```bash
cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREMORA_FUZZ=ON
cmake --build build-fuzz --target remora_fuzz_device_message_parser
ctest --test-dir build-fuzz --output-on-failure -L fuzz
```

The `fuzz`-labelled smoke test fuzzes for about ten seconds under ASan and UBSan, starting from the
checked-in seeds, and fails on any sanitizer report or broken invariant. Inputs it discovers and
crash artifacts are written under `build-fuzz/fuzz/`, never into the source tree. A crash becomes a
pin test in `tests/test_mirrorproto.cpp`.

| Target | Parser | Corpus |
|---|---|---|
| `remora_fuzz_device_message_parser` | `DeviceMessageParser` | `corpus/device_message_parser/` |

## Seeds

Every seed is a hex vector from `tests/test_mirrorproto.cpp`, named after the test it comes from:
complete, truncated and concatenated records, plus the framing violations the pin tests cover.
`make_seeds.py` holds the table and writes the files; rerun it after changing the table.
