# Security Model

This page is a short, user-facing summary of what Remora trusts and what follows from that. It
is drawn from [SECURITY.md](../../SECURITY.md), which is the authoritative text and also explains
how to report a vulnerability.

## What Remora trusts

Some things that look like vulnerabilities are the design. Know these before you decide where to
run an instance and what to put in it.

| boundary | what it means for you |
|---|---|
| **The Android container runs `--privileged`.** Android's init, its binder devices and GPU access need it | Root inside the container is close to root on the docker host. Run images you built, from sources you trust, and treat a compromised Android instance as a compromised host |
| **Docker group membership is root-equivalent** on the machine that runs the containers | Remora requires it for the `bare` target. That is Docker's model, not something Remora adds |
| **adb is a root shell into the device**, reachable on the instance's adb port | Anything that can reach the port can use adb against the device. Control who can reach it |
| **The mirror agent listens on an abstract unix socket inside the container**, reached through an adb forward | It trusts any client that reaches it that way. adb's own key-based authentication stays the trust boundary |
| **`remote` targets are driven over ssh** | with your own keys and ssh configuration |

## Who can reach adb

| setup | adb is reachable from | what to do |
|---|---|---|
| `bare`, default | this machine only: the port is published on loopback | nothing |
| `remote`, default | every interface of the docker host, because the client connects from another machine | narrow it with `adb_bind=<address>`, or firewall the port |
| `network_mode=macvlan` | anything on your LAN: the instance has its own address there | be aware of who shares that network |
| `adb_bind=0.0.0.0` | every interface, on either target | firewall the port if that is wider than you need |

A changed `adb_bind` applies when the container is next recreated. See
[Deploy Targets](Deploy-Targets.md#adb-exposure).

## Things Remora keeps private

- The Google checkin identity that Remora keeps per instance is readable by its owner only, and so
  is its directory.
- The Reddit session cookie used by `remora reddit-login` never appears on a command line when you
  pass `--cookie -`: it is read from stdin and travels through private files. Passing the value as
  an argument also works, but shows it to other users of the host through `ps`.

## Images and downloads

- **Android images are not release assets.** You build your own, from sources you can inspect.
- **Third-party payloads are yours to supply.** `vendor/fetch-payloads.sh` checks each download
  against a recorded hash before it reaches the tree, and refuses to guess a source it does not
  know.
- **Release downloads carry a build-provenance attestation.** A pass means the file came from the
  project's release workflow and has not been altered. Provenance is not code signing, and a
  `PKGBUILD` is a recipe your machine runs, so read it first. See
  [Installation](Installation.md#verifying-a-download).

## The diagnostics bundle

`remora doctor` writes your profile's settings and the device's logcat into one file. Read it and
redact it before you attach it to a public issue.

## Reporting a vulnerability

Do not open a regular issue for a suspected vulnerability: issues are world-readable the moment
they are filed. Open a private security advisory on the repository instead. Include a description,
steps to reproduce, the impact as you understand it, and a suggested fix if you have one. The
project commits to an acknowledgement within 48 hours.

A way to cross one of the boundaries above that the list does not describe is exactly what to
report: for example a process in the container reaching the host without `--privileged` being the
reason, or the mirror client being made to run code by a malicious device.
[SECURITY.md](../../SECURITY.md) has the link and the full policy.
