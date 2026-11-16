# Qt CBOR-to-JSON conversion: exposure audit

This page records whether Remora's own source has a code path that matches a reported Qt Core
defect. The advisory itself was **not verified**: no network was available for the audit, and
nothing here confirms the CVE identifier, the description, or the affected versions.

## The report (unverified)

- Identifier as reported: CVE-2026-105924.
- Reported trigger: a NULL dereference when CBOR decoded with `QCborValue` is converted to
  `QJsonValue`, `QJsonArray` or `QJsonObject`.
- Reported affected ranges, taken only from the Qt wiki list and not checked: Qt 5.12.0–6.8.9,
  6.9.0–6.11.2 and 6.12.0.

Remora builds against a system Qt: `CMakeLists.txt` asks for `find_package(Qt6 6.4 REQUIRED ...)`.
A distribution Qt inside the reported ranges is therefore possible. What matters for Remora is
whether its own code ever decodes CBOR and converts it to JSON types.

## Search

Run from the repository root at the audited revision
(`git rev-parse HEAD` = `ab082ad87e678614429bdecc4a351cbc0eed6c3d`):

```
git grep -n -I -E 'QCbor|fromCbor|toCbor' -- src vendor
```

- Directories searched: `src/` and `vendor/` (tracked files only, binaries skipped by `-I`).
- Result: no output, **exit status 1**. For `git grep`, exit status 1 means "no match"; an
  execution error would be exit status 2 or higher, and none occurred.

A wider case-insensitive check, `git grep -n -I -i 'cbor'` over the whole tracked tree, finds only
four lines, in `vendor/source-build/pinned-manifest-a16.xml` and
`vendor/source-build/pinned-manifest-a17.xml`. They name the Android source projects
`platform/external/cbor-java` and `platform/external/cn-cbor`. Those entries only fetch source for
the Android image build. They are not Qt, they are not linked into the `remora` binary, and they
do not match the reported trigger.

## Where Remora does use JSON

Every JSON path in Remora's own code is **text** JSON, parsed by `QJsonDocument::fromJson` or
written by `QJsonDocument::toJson`. None of them starts from CBOR:

| Location | Function | Input |
|---|---|---|
| `src/core/Gating.cpp` | `stalePifProfile` | PIF profile JSON text, passed in by the caller |
| `src/core/RedditSession.cpp` | `redditAccountIdFromCookie` | base64url-decoded JWT payload from a session cookie |
| `src/ui/RedditLoginWindow.cpp` | `nameFromMeJson` | `/api/me.json` response body |
| `src/engine/PublicImages.cpp` | `publicImagesResult` | a tagged line of catalog helper output |
| `src/engine/PlayInstall.cpp` | `manifest`, `playInstallResult` | the vendored `play/package.json`; a tagged line of installer output |
| `src/ui/MainWindow.cpp` | `scanPrunable` | `docker system df -v --format '{{json .Images}}'` output, walked as `QJsonValue` items of the array |
| `src/engine/Probe.cpp` | the probe report serialiser | builds `QJsonObject`s and writes them with `toJson`; nothing is parsed |

`scanPrunable` is the only place that walks `QJsonValue` items. Its document also comes from
`fromJson` on text, not from `QCborValue::toJsonValue`. Some of these inputs come from outside the
process, such as a web response, a cookie, or a remote host's docker output. They are still text
JSON, and text JSON is not the reported trigger. A search for `QJsonValue`, `QJsonArray`,
`fromVariant`, `toVariant()`, `fromBinaryData` and `QBinaryJson` across `src`, `vendor` and `agent`
finds only the `MainWindow.cpp` lines above.

## Conclusion

The audited tracked Remora sources contain no identified CBOR decode or CBOR-to-JSON conversion
path for this reported trigger. No call site exists, so no input trust boundary had to be
assessed.

This conclusion is narrow. It does not say that Qt itself is safe, and it does not cover code
inside Qt or other libraries that Remora does not call directly. It makes no runtime, device or
hardware guarantee. It also does not confirm the advisory or its version ranges. If a later change
adds `QCborValue`, `QCborStreamReader`, `fromCbor` or `toCbor`, run the search again and check
whether that input can be untrusted.
