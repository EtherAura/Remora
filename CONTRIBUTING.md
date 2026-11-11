# Contributing to Remora

Thank you for considering a contribution. Bug reports, fixes, documentation and new hardware
reports are all welcome.

## Reporting a bug

Use the **Bug report** form. The single most useful thing to attach is a diagnostics bundle:

```sh
remora doctor <profile>
```

It collects the resolved configuration, the host probes and readiness, versions, and the device's
state and recent log into one file.
**Read it before attaching it** — it contains your profile's settings and the device's logcat, which
can include things you would rather not publish. Redact freely.

Hardware matters a great deal here (GPU vendor and driver, kernel, distribution), so please say
what you run on — especially if it is something other than Intel, AMD or NVIDIA on a recent kernel.

## Asking a question

Questions, setups that work and ideas that are not yet a bug or a feature request go to
[Discussions](https://github.com/EtherAura/Remora/discussions). The
[user guide](https://github.com/EtherAura/Remora/wiki) may already have the answer.

## Development setup

The README's [What you need](README.md#what-you-need) lists the build dependencies per
distribution. Then:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The eight test suites run offline with **no hardware**: nothing touches Docker or a device, because
the process layer is injectable (`FakeSpawner` in `tests/test_engine.cpp`).

You do not need to build an Android image to work on the desktop side. You do need one to test the
mirror, deploys or anything under `vendor/source-build/` end to end.

## How the code is organised

- **`src/core/` stays pure** — Qt Core only, no `QProcess`, no filesystem, no Widgets. Anything
  impure goes behind the injectable `Spawner` in `src/engine/`, so it can be tested offline.
- **Configuration is `std::optional<T>` everywhere.** Unset means "the resolver applies the proven
  default", and an empty configuration must resolve to a bootable instance. Defaults live in
  `Resolver`, never in the config model.
- **Command builders are golden-tested.** Changing the `docker run` or mirror argv in `Builders.cpp`
  means updating `tests/test_builders.cpp`; `remora plan` output is a user-facing contract.
- **The mirror's wire format is a contract** — `docs/MIRROR_PROTOCOL.md`, pinned byte for byte by
  `tests/test_mirrorproto.cpp`. A change to it is a protocol version bump on both sides.
- **Deploy targets are `bare` and `remote`.** Prefer changes that work on both over ones that
  special-case one.
- **Third-party binaries are never committed.** Payloads such as Google apps are described in
  `vendor/PAYLOADS.md` and fetched or supplied by the user.

Match the surrounding style: `QStringLiteral` for literals, `camelCase` members with a trailing `_`,
and comments that explain *why* at the density of the file you are in.

## Submitting changes

1. Fork, and branch from `main` — one branch per logical change.
2. Keep the tests green (`ctest --test-dir build`) and add tests for new behaviour where it can be
   tested offline.
3. Shell scripts are linted in CI with `shellcheck --severity=warning` and parsed with the shell
   their shebang names; run both before pushing.
4. A change users can notice (anything under `src/`, `agent/` or `vendor/`) adds its entry under
   `[Unreleased]` in `CHANGELOG.md`, written for the people who use Remora. A change nobody
   outside can notice says so instead: ask for the `no-changelog` label on the pull request, or
   put `[no-changelog]` alone on a line of the commit message. `tools/check-changelog.sh
   origin/main` is the check CI runs.
5. The user guide is `docs/user/`, one page per file, published to the wiki on every push to
   `main`. A change to what a command or a setting does changes its page in the same pull request.
6. Open a pull request describing what changed and how you verified it. For anything that needs
   hardware to verify, say what you ran it on.

## What CI checks

| check | what it is |
|---|---|
| build + test | a Release build and the eight suites |
| sanitizers | the same suites under AddressSanitizer and UndefinedBehaviorSanitizer |
| coverage | how much of each tested layer the suites execute, against a floor (`tools/coverage.sh`) |
| shell scripts | `shellcheck --severity=warning`, and every script parsed by the shell it names |
| desktop metadata | the AppStream metainfo and the desktop entry validate |
| no third-party binaries | nothing binary is committed (see `vendor/PAYLOADS.md`) |
| changelog entry | the rule in step 4 |

CodeQL reads the C++, the agent's Java, the Python helpers and the workflows on every push to
`main` and once a week; what it finds appears as a code scanning alert, not as a failed check.

## Licence

Remora is GPL v3. By contributing you agree that your contribution is licensed under the same terms.
