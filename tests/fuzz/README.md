# Fuzz targets

libFuzzer harnesses for the mirror protocol's parsers of guest-controlled bytes. Off by default,
Clang only, and built in a directory of their own; the ordinary GCC build and its test run never
see them.

```bash
cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREMORA_FUZZ=ON
cmake --build build-fuzz --target remora_fuzz_device_message_parser remora_fuzz_parse_session_reply remora_fuzz_text_message
ctest --test-dir build-fuzz --output-on-failure -L fuzz
```

Each `fuzz`-labelled smoke test fuzzes for about ten seconds under ASan and UBSan, starting from the
checked-in seeds, and fails on any sanitizer report or broken invariant. Inputs it discovers and
crash artifacts are written under `build-fuzz/fuzz/`, never into the source tree. A crash becomes a
pin test in `tests/test_mirrorproto.cpp`.

| Target | Parser | Corpus |
|---|---|---|
| `remora_fuzz_device_message_parser` | `DeviceMessageParser` | `corpus/device_message_parser/` |
| `remora_fuzz_parse_session_reply` | `parseSessionReply` | `corpus/parse_session_reply/` |
| `remora_fuzz_text_message` | `textMessage` (encoder) | `corpus/text_message/` |

The `parseSessionReply` harness checks that a reply never consumes more than the buffer holds,
that it is complete at exactly that many bytes and not one fewer, and that passing no `consumed`
pointer changes nothing.

The `textMessage` harness is an encode→parse round trip. It reads the record back by hand (u32
big-endian size equal to the bytes after it and at most the 1 MiB control limit, type `0x02`) and
through `RecordReader` at that limit, then requires the body to be the text's UTF-8 when that fits
in 1 MiB - 1 bytes, and otherwise the longest prefix of at most that many bytes that ends on a
codepoint boundary. The body is always valid UTF-8, and encoding the decoded body again gives the
same record. Its input is not wire bytes: the first byte picks the mode. Below `0x80` the rest is
the text, through `QString::fromUtf8`; from `0x80` up the rest is a prefix length byte, that many
bytes of prefix, then a unit repeated until the text's UTF-8 reaches 1 MiB - 1 - 64 +
(mode & `0x7f`) bytes, so every run near the truncation boundary stays around 1 MiB.

## Seeds

## Seeds

Every seed is a hex vector from `tests/test_mirrorproto.cpp`, named after the test it comes from:
for `DeviceMessageParser`, complete, truncated and concatenated records, plus the framing
violations the pin tests cover; for `parseSessionReply`, the vectors of
`agentSessionReplyParsing` (success and failure replies, complete, truncated and followed by more
of the stream) and one empty input. The `textMessage` seeds are harness inputs that rebuild the
texts of the two pin tests exactly: `textGolden` is `00` + `68c3a9` ("hé");
`textTruncationGolden-straddle` is `c2 01 61 f09f9880`, "a" then U+1F600 × 262144, cut back to
"a" + 262143 of them; and `textTruncationGolden-fit` is `c0 00 61`, "a" × 1048575, which fits
exactly.
`make_seeds.py` holds the table and writes the files; rerun it after changing the table.
