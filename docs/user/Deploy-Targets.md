# Deploy Targets

This page explains where an Android instance can run (`bare` or `remote`), what each target needs,
how to check that a target is ready, how the instance is networked and how its adb port is
exposed, and how to run several instances side by side.

## The two targets

Both targets run the same container with the same `docker run` arguments. They differ only in
where docker lives and in a few default paths.

| target | where docker runs | chosen when | adb target |
|---|---|---|---|
| `bare` | this machine | `ssh_host` is empty | `localhost:<host_adb_port>` |
| `remote` | any docker host reachable over key-based ssh | `ssh_host=user@host` is set | `<host>:<host_adb_port>` |

The target is inferred from `ssh_host`. An explicit `backend=bare` or `backend=remote` in the
profile overrides the inference, and `--backend` on the command line overrides both. An unset or
unrecognised value means `bare`.

Both targets can render on a real GPU: see [GPU Acceleration](GPU-Acceleration.md).

## What each target needs

| target | required |
|---|---|
| both | `adb` on the machine you run Remora on |
| `bare` | your user in the `docker` group, a reachable docker daemon, and for Android 16 images the IBT-fixed `ashmem_linux.ko` loaded |
| `remote` | key-based ssh to `ssh_host`, and docker usable by the remote user |

The docker host's kernel must also provide binder (binderfs).

## Checking readiness

`remora check` probes the target read-only and reports what is missing, with a remedy for each
row. It does not install anything.

```sh
remora check                                        # the active profile
remora check <profile>                              # a named profile
remora check --backend remote --ssh-host user@host  # a remote target, naming the ssh host
remora check --gpu-host                             # add the GPU rows
```

The target is ready when every *required* row passes. The other rows are informational. The
command exits 0 when the target is ready and 1 when it is not.

| row | applies to | severity | remedy |
|---|---|---|---|
| `adb` present | all | required | install android-tools |
| user in the `docker` group | bare | required | `usermod -aG docker $USER`, then log in again |
| docker daemon reachable | bare | required | start docker, or unset a stale `DOCKER_HOST` |
| IBT-fixed `ashmem_linux.ko` loaded | bare, Android 16 only | required | load the module. Android 17 needs none |
| remote reachable over ssh | remote | required | set up key-based ssh to `ssh_host` |
| docker usable on the remote | remote | required | install docker, add the remote user to its group |
| host LAN readable | `network_mode=macvlan` | required | set `macvlan_parent`, `macvlan_subnet` and `macvlan_gateway`, or use `bridge` |
| `python3` on the docker host | `shared_inputs` set | optional | install python3 |
| GPU rows | `gpu_mode=host` or `--gpu-host` | informational | see [GPU Acceleration](GPU-Acceleration.md#checking-the-gpu) |

Below the rows, `check` adds information that does not change the verdict: how `/data` is
delivered and what is on disk, the image's CPU tuning against the target, and whether the build
assets and patch tree for each Android version are present. A line beginning `DEPLOY WILL REFUSE`
tells you about a refusal before you run into it.

`remora plan` is the companion dry run: it prints the resolved `docker run` and mirror commands
for a profile without deploying anything.

## Per-target defaults

A profile created in the workspace is given its own container name (`remora-<profile>`), data
directory (`~/.remora-<profile>`) and the lowest free host adb port from 5555 upward, written into
the profile. A hand-written profile that sets none of these gets:

| key | `bare` | `remote` |
|---|---|---|
| `container_name` | `remora-<profile>` | `remora-<profile>` |
| `data_dir` | `~/.remora-bm-data` | `~/remora-data` (on the remote host) |
| `data_base_dir` | `~/.remora-data-base` | `~/remora-data-base` (on the remote host) |
| `host_adb_port` | 5555 | 5555 |
| `adb_bind` | `127.0.0.1` | every interface |
| `network_mode` | `bridge` | `bridge` |

The container name is also how Remora finds a running instance, so renaming one is a deliberate
edit followed by `remora up`.

## adb exposure

adb is a root shell into the device. Anything that can reach the instance's adb port can use it.

| target | default | why |
|---|---|---|
| `bare` | published on `127.0.0.1` only | the instance is reached from this machine only |
| `remote` | published on every interface of the docker host | the client connects from another machine |

`adb_bind=<address>` narrows or widens either. `adb_bind=0.0.0.0` publishes everywhere. On a
`remote` target, narrow it to the interface the client comes in on, or firewall the port. A
changed setting applies when the container is next recreated.

## Networking

| `network_mode` | what you get | adb target |
|---|---|---|
| `bridge` (default) | the adb port is published on the docker host | `<host>:<host_adb_port>` |
| `macvlan` | the container has its own address on your LAN | the container's own address, port 5556 |

With `macvlan`, other machines see the instance as a device on the network, and anything on that
network can reach it. Remora derives the network from the docker host's default route (parent
interface, subnet and gateway) and creates the docker network on the first deploy. The keys
`macvlan_parent`, `macvlan_subnet`, `macvlan_gateway`, `macvlan_ip`, `macvlan_range` and
`docker_network` override that. Addresses are assigned from the top /28 of the subnet by default:
check that the slice is outside your router's DHCP pool, or set `macvlan_range`.

One kernel rule matters on `bare`: a macvlan interface and its parent cannot exchange traffic, so
the machine hosting the container cannot reach it directly. Remora handles this by keeping a
host-side shim interface and routes, reconciled on every connect, using only the docker-group
access `bare` already requires. Nothing persistent is installed. Set `macvlan_host_route=false` to
manage host routing yourself. The shim carries unicast only, so discovery by broadcast or
multicast does not find the container from its own host. Details are in
[the macvlan shim README](../../vendor/host-prereqs/macvlan-shim/README.md).

`dns` sets up to four DNS servers. `proxy_type`, `proxy_host` and `proxy_port` set a system proxy.

## Several instances side by side

Each profile is one instance. Instances can run at the same time as long as they do not share a
container name, a data directory or a host adb port. Profiles created in the workspace get their
own of each. A hand-written profile gets a container name derived from its own name, but the
default data directory and adb port are the same for every profile, so set `data_dir` and
`host_adb_port` yourself.

By default every instance keeps its `/data` as a private layer (`data_dir`) over a shared,
read-mostly base (`data_base_dir`) that the profiles on one host stand on. `use_overlayfs=false`
gives a profile a plain bind of its own data directory.

When a deploy finds another instance already holding something the profile needs, `remora up`
stops and tells you which instance is using what. Stop that instance, or give this profile its
own, then retry.

To start an instance over, `remora reset-data <profile> --yes` deletes the profile's `/data`
(`bare` only). With the default layering, only that instance's layer is removed and the shared
base stays.

## The same profile on several hosts

The workspace's Runners page keeps deployments: a profile summoned on a specific docker host,
optionally with a different image. They are stored as `[Deployment-<name>]` sections, described in
[Configuration](Configuration.md#other-sections).
