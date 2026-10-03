# Security Policy

## Reporting a vulnerability

**[Open a private security advisory](https://github.com/EtherAura/Remora/security/advisories/new).**
Private vulnerability reporting is enabled on this repository: the report stays between you and
the maintainer until an advisory is published, the report, discussion and fix live on one thread,
and it is the route that can request a CVE and credit you by name.

Please **do not** open a regular issue for a suspected vulnerability — issues are world-readable
the moment they are filed.

Please include a description, steps to reproduce, the impact as you understand it, and a suggested
fix if you have one. You will get an acknowledgement within 48 hours, and security fixes are
prioritised over everything else.

## What Remora trusts, and what that means

Knowing these makes a report easier to judge — and some things that look like vulnerabilities are
the design:

- **The Android container runs `--privileged`.** Android's init, its binder devices and GPU access
  need it. Root inside the container is therefore close to root on the docker host: run images you
  built, from sources you trust, and treat a compromised Android instance as a compromised host.
- **Docker group membership is root-equivalent** on the machine that runs the containers. Remora
  requires it for the `bare` target; that is Docker's model, not something Remora adds.
- **adb is a root shell into the device, reachable on the instance's adb port.** A `bare` instance
  publishes it on loopback only, so only this machine can connect. A `remote` instance publishes it
  on every interface of the docker host, because the client connects from another machine — narrow
  it with `adb_bind=<address>` or firewall it. `network_mode=macvlan` gives the instance its own
  address on the LAN, reachable by anything on that network. Anything that can reach the port can
  use adb against the device.
- **The mirror agent listens on an abstract unix socket inside the container**, reached through an
  adb forward. It trusts any client that reaches it that way.
- **`remote` targets are driven over ssh** with your own keys and ssh configuration.

A way to cross one of these boundaries that the list does not describe — for example a process in
the container reaching the host without `--privileged` being the reason, or the mirror client being
made to run code by a malicious device — is exactly what to report.

## Verifying a release download

Every release asset this project builds — the source tarball and the `.deb` — carries a
[Sigstore-backed build provenance attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations/using-artifact-attestations-to-establish-provenance-for-builds)
recording which workflow, at which commit, produced those exact bytes:

```bash
gh attestation verify remora_<version>_amd64.deb --repo EtherAura/Remora
```

A pass means the file came from this repository's release workflow and has not been altered since.
That is stronger than the `.sha256` files beside each asset, which only prove a file matches a hash
published on the same page. Each release also publishes an SBOM of what went into it.

**Provenance is not code signing**, and it covers what the workflow built: the `PKGBUILD` is a
recipe your machine runs, so read it before running `makepkg`.

**Android images are not release assets.** You build your own, from sources you can inspect; see
the README's *What you need*.
