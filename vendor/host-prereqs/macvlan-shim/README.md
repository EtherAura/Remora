# macvlan shim — reaching your own containers

`network_mode=macvlan` gives an instance its own address on your LAN: other machines see it as a
device on the network, it answers on its own IP, and it can be discovered by anything that relies
on broadcast or mDNS. Remora derives the network from the docker host itself (the parent NIC, the
subnet and the gateway come from that host's default route) and creates the docker network on the
first deploy, so nothing has to be typed by hand.

`network_mode=macvlan` works the same on both backends. There is one kernel rule to know about,
and it only bites on `bare`.

## The rule

A macvlan sub-interface and its **parent** cannot exchange traffic. Every other machine on the LAN
reaches the container normally; the machine hosting it does not. On `remote` this is invisible —
the docker host is a different machine from the one running adb and the mirror, and the traffic
crosses the real LAN. On `bare` they are the same machine, so adb may not be able to reach the
container it just started.

Whether you actually hit this depends on the host:

- **A second interface on the same LAN** (say wifi alongside ethernet) usually sidesteps it — the
  host sends to the container out the other NIC and the switch hairpins the frame back. This works
  and needs nothing installed, but it stops working the moment that second interface goes down.
- **A single-NIC host** hits it every time.

## The fix — and Remora does it for you

Give the host a macvlan child of its own on the same parent and route the container addresses
through it. Two macvlan children of one parent *can* talk; only the parent itself is fenced off.

**Remora owns this interface.** Every connect to a macvlan profile on `bare` reconciles it before
the first adb handshake — it does not merely create one if none exists:

- wrong parent NIC, or the wrong **mode**, and the interface is torn down and rebuilt (neither can
  be changed on a live device);
- the shim's address and every wanted route are re-asserted;
- routes that are no longer wanted are **withdrawn**, so a re-pinned container does not leave the
  host routing an address nothing answers on.

A correct shim is left alone, because rebuilding one drops the adb connection riding it.

No password is involved: the commands run in a privileged container sharing the host's network
namespace, which needs only the docker-group membership `bare` already requires (`preflight` checks
for it) — and docker-group access is root-equivalent by construction, so nothing is escalated, only
used. The deploy log says what it is doing to host networking.

Nothing persistent is installed. The shim disappears on reboot and Remora rebuilds it on the next
connect. Its address defaults to the one below the auto-IP pool and is checked against the LAN
before first use; if something already answers there, the deploy stops and asks for
`macvlan_shim_ip=`.

Turn it off with `macvlan_host_route=false` if you would rather manage host routing yourself.

### Running it by hand

The script is for the `macvlan_host_route=false` case, and for inspecting or tearing down what
Remora built:

```bash
sudo sh vendor/host-prereqs/macvlan-shim/remora-macvlan-shim status
sudo sh vendor/host-prereqs/macvlan-shim/remora-macvlan-shim install <parent-nic> <cidr> [<cidr>...]
```

`remora plan` prints the parameters for the current profile. The script installs **no** systemd
unit: two things owning one interface is how it ends up in a state neither expects. If Remora sees
a `remora-macvlan-shim.service` on the host it says so, because that unit and the automatic path
are both trying to own `remora-shim0`. Remove the unit, or set `macvlan_host_route=false` to hand
ownership back to it.

`remove` deletes the interface; Remora will rebuild it on the next connect unless you have opted
out. Removing it is a reset, not an opt-out.

### One trap worth knowing

`mode bridge` is what makes sibling macvlan devices able to reach each other. The default, VEPA,
expects frames to be reflected by an 802.1Qbg-capable switch — which ordinary networks are not. A
VEPA shim comes up, takes its address, accepts its routes, and looks perfect under `ip link` while
reaching nothing at all; only `ip -d link` shows the mode. Real iproute2 honours `mode bridge`;
busybox's `ip` accepts the argument and silently ignores it, which is why Remora runs the host's
own `ip` through a chroot rather than the one in the helper image.

## Addressing

Docker's IPAM knows nothing about your router's DHCP pool. Handed a whole `/24` it starts at `.2`,
which is usually inside the lease range — so Remora defaults `--ip-range` to the **top /28** of the
subnet (`192.168.0.240/28` for a `192.168.0.0/24` LAN), the conventional static end of a home
network. Check that slice is outside your DHCP pool, or set `macvlan_range=` to one that is.

A pinned `macvlan_ip=` is validated against the subnet, not the range, so you can pin any free
address on the LAN.

## Config keys

All optional; each falls back to the live probe of the docker host.

| key | meaning | default |
| --- | --- | --- |
| `network_mode` | `macvlan` or `bridge` | `bridge` |
| `docker_network` | network name | `lan` |
| `macvlan_ip` | pinned container address | unset → docker IPAM assigns from the range |
| `macvlan_parent` | parent NIC | the docker host's default-route interface |
| `macvlan_subnet` | subnet CIDR | that interface's own network |
| `macvlan_gateway` | gateway | the default gateway |
| `macvlan_range` | auto-assignment pool | the top /28 of the subnet |
| `macvlan_host_route` | let Remora assert the host route each connect | `true` (`bare` only) |
| `macvlan_shim_ip` | the shim interface's own address | one below the auto-IP pool |

## What macvlan does not give you

The shim carries **unicast**. Broadcast and multicast between the host and its own macvlan
container stay blocked by the same isolation rule, so anything that finds devices by broadcasting
— KDE Connect's discovery, mDNS/Avahi, Chromecast — will not see the container *from this machine*.
Every other device on the LAN discovers it normally, because their traffic crosses the real
network. Where an app allows a device to be added by address, use the container's IP directly:
KDE Connect, for instance, pairs fine over its custom-device list once you name the address.
