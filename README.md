# ax88179-gadget

Make a Linux box pretend to be an **ASIX AX88179 USB Gigabit Ethernet
adapter**, using `raw-gadget` from userspace.

The emulated adapter is a real USB device as far as the host is concerned:
it enumerates, runs the chip's register protocol, reports link status and
carries Ethernet frames. Any host with an AX88179 driver binds to it
without modification.

## Where this comes from

Written from scratch, out of public material only:

* the AX88179 datasheet — registers, endpoint assignments, descriptor
  layout;
* the public Linux `ax88179_178a` and FreeBSD `if_axge` driver sources,
  read as a specification for the wire protocol;
* and an unreasonable amount of trial and error against real hosts.

No firmware was reversed, and no real AX88179 was ever captured. The
device was written from the driver's point of view and then corrected
until three unrelated operating systems accepted it — which is decent
evidence that it is right, but it is not a soak test:

> **No guarantee of stability, throughput, or behaviour under load.**
> Treat this as a working prototype, not a production NIC.

## Legal

An interoperability exercise: a new device that happens to speak an
existing chip's protocol. It is not affiliated with, authorised by, or
endorsed by ASIX Electronics, and it contains no ASIX code, firmware, or
other confidential material.

"ASIX" is a trademark of ASIX Electronics Corporation. "AX88179" is not a
trademark — it is the product model code ASIX uses for this chip. Both are
used here only to describe what this program emulates.

---

> ## ⚠️ Read this first: boot with nothing attached
>
> Some bootloaders probe the USB port for a flashing tool **before** Linux
> starts. If a USB host is already attached at power-on, the bootloader can
> wait for that tool's handshake forever, and the board never boots at all —
> no network, no login, nothing.
>
> Confirmed on an **Amlogic S805 board with its vendor U-Boot**, which stops
> here and stays there indefinitely:
>
> ```
> U-Boot
> Checking USBBurn...
> ```
>
> **Power the board first, then attach the host device** — or turn the host
> on afterwards. Nothing is lost by connecting late: this program only starts
> doing anything once Linux is running, and the emulated NIC appears as soon
> as it is plugged in.
>
> This is a bootloader quirk, **not a bug in this program**. The same trap
> exists on other Amlogic boards, and other SoCs or bootloaders may have
> their own version of it. If a board refuses to boot while a USB device is
> attached, try booting it with the port empty.

---

Tested hosts:

| Host | Driver | Result |
| --- | --- | --- |
| Linux | `ax88179_178a` | works |
| Windows 10/11 | `netax88179_178a` 1.18.19.1128 | works |
| Nintendo Switch | `eth` sysmodule | works |

## Why

A device that only accepts a specific USB Ethernet chipset is a common
situation — consoles are the usual example. If the device you want to
network cannot act as a USB host to a generic adapter, you can put a small
Linux board in between and have it *be* the adapter.

```
[ host ]──USB──[ linux board: ax88179-gadget ]──ethernet──[ network ]
```

The board also gives you a place to run NAT, firewalling or a DHCP server,
so the attached host can live on its own subnet.

## Requirements

* Linux with `CONFIG_USB_RAW_GADGET` (`=y` or `=m`) — **many distribution
  kernels do not enable this**; check with
  `zgrep USB_RAW_GADGET /proc/config.gz` or `modinfo raw_gadget`.
* A USB device controller (UDC); check `ls /sys/class/udc`.
* `/dev/net/tun` (`CONFIG_TUN`).
* root.

For the optional downstream subnet: `dnsmasq`, `iptables`, `udev`.

## Build

```sh
gcc -O2 -pthread -o ax88179_gadget ax88179_gadget.c
```

## Run

```sh
sudo ./ax88179_gadget [udc-name]
```

With a single UDC the name is auto-detected. The program:

* binds the emulated device to that UDC,
* creates a TAP interface named `ax88179` and gives it `10.10.10.1/24`,
* forwards frames in both directions until it is stopped.

Environment:

| Variable | Meaning |
| --- | --- |
| `AX_MAC` | MAC reported to the host (default `02:00:00:00:00:01`) |
| `AX_PAD` | force the RX alignment padding (`0` or `2`) instead of following the host's `AX_RX_CTL.IPE` setting |

## Downstream network (optional)

The gadget only moves frames. To give the attached host an address and
internet access, run the setup script **once**:

```sh
sudo ./deploy/setup-network.sh --uplink eth0
```

> If you copied the tree instead of cloning it (for example over `scp`
> from Windows, where the executable bit is not preserved), either
> `chmod +x deploy/setup-network.sh` first or run it as
> `sudo bash deploy/setup-network.sh --uplink eth0`.

It enables `ip_forward`, adds NAT rules towards `--uplink`, configures
dnsmasq for `10.10.10.50` and installs a udev rule that restarts dnsmasq
whenever the TAP interface appears.

Everything it writes is persistent: `sysctl.d`, `dnsmasq.d`, `udev.rules`
and the saved firewall rules all survive a reboot. **You do not need to
run it again after rebooting** — and running it again is harmless, it is
idempotent and asks before replacing any file.

The script never installs packages and never overwrites an existing
config file without showing a diff and asking.

> **Firewall persistence needs a separate package.** The rules are only
> restored on boot if `netfilter-persistent` is installed (Debian/Ubuntu:
> `apt install iptables-persistent`). If it is missing the script warns,
> and then **you have to re-run this script after every reboot** —
> otherwise the host gets an address but no internet.

## Install as a service

```sh
sudo install -m 0755 ax88179_gadget /usr/local/bin/
sudo install -m 0644 deploy/ax88179-gadget.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ax88179-gadget
```

Verified from systemd: the service starts, the host gets an address and
can browse. **Booting with the service enabled has not been verified yet** —
in particular the ordering between the TAP appearing, the udev rule firing
and dnsmasq rebinding.

## How it works

`raw-gadget` hands userspace the device side of USB: descriptors, control
requests and bulk transfers. The program therefore implements what the
chip would do:

* **descriptors** — vendor-specific device, one interface, interrupt IN
  (link status) plus bulk IN/OUT (frames);
* **register model** — `AX_ACCESS_MAC` reads/writes are stored in an
  array, PHY reads return a fixed link-up state, EEPROM reads return
  `0xff` so the driver falls back to its defaults;
* **interrupt endpoint** — a link-up notification is sent every second;
* **bulk OUT** — the 8 byte AX88179 TX header is stripped and the frame is
  written to the TAP;
* **bulk IN** — frames read from the TAP are wrapped in the AX88179 RX
  format (metadata entry, trailer with packet count and offset).

One detail that is easy to miss and breaks everything: the driver enables
**TX checksum offload**, so the frames it sends have an IPv4 header
checksum of `0` and a partially computed L4 checksum, expecting the chip
to finish them. `fix_tx_checksums()` does that; without it the host stack
drops every TCP/UDP frame (`IpInHdrErrors`) while DHCP still works,
because DHCP clients commonly send a zero UDP checksum.

## Limitations

* Requires `CONFIG_USB_RAW_GADGET`; on stock distribution kernels this is
  often missing and means building a kernel.
* Needs root.
* TAP name is fixed to `ax88179`.
* MTU is 1500; jumbo frames are not handled.
* A single USB transfer is limited to 2048 bytes, so very large GSO
  frames are truncated.
* Only the vendor-specific configuration of the real chip is emulated
  (the real device also exposes CDC-NCM and CDC-Ether configurations).
* One interface, one configuration.
* The auto padding mode trusts that the driver writes `AX_RX_CTL` before
  it starts receiving.
* Hot-swapping hosts works: unplug one host, plug another, without
  restarting the service. The TAP and its address survive, so the new host
  gets `10.10.10.50` again. Verified Switch → Windows. (The worker threads
  are not joined on disconnect, so restart the service if a swap ever
  misbehaves.)
