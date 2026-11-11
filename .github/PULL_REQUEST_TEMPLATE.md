## What changed

<!-- One logical change per pull request. -->

## How it was verified

<!-- `ctest --test-dir build` result, and for anything hardware-dependent: what you ran it on
     (GPU + driver, distribution, Android version, bare/remote). -->

- [ ] `ctest --test-dir build` passes
- [ ] Golden argv tests updated if `remora plan` output changed
- [ ] Shell scripts pass `shellcheck --severity=warning`
- [ ] `CHANGELOG.md` has an entry under `[Unreleased]`, or nobody outside can notice the change
      (`no-changelog`)
- [ ] The user guide (`docs/user/`) says what the change makes true
