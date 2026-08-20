# Replacement Driver for IGS PCI PCCard

This project provides a replacement Linux kernel driver for various IGS games that used an IGS PCI card as their primary I/O board.

These PC-based games originally ran on a customised RHEL 3 system, the required driver was built directly into the kernel. The original kernel module can be recovered from the game's disk.

The driver has been verified and tested on Ubuntu 10.04 LTS and Debian 10. While the driver may work on newer kernels, newer systems may cause the game to malfunction due to incompatibilities with its legacy SDL implementation. 

## Platform Setup

This section describes the platform and software configuration required to build and use the replacement driver.

- Disable APIC in the BIOS if available, or boot Linux with `noapic`. With APIC
  enabled, some newer boards route/latch PCI/CardBus interrupt state in a way
  that can hard-lock the game even when the protocol tests pass.
- Set BIOS PCI Plug and Play / PnP OS handling to the legacy/non-OS-managed
  mode. On the tested boards this meant `PCI PnP OS: Off` / `PnP OS: No`.
  The card and driver behave best when the BIOS assigns conventional PCI
  resources before Linux boots.

## PCCARD module

Loadable-module version of the patched PCI PCCARD driver.

Linux 2.6 and later use kbuild and produce `pccard.ko`:

Debian 10 / Linux 4.19 i386 notes:

```sh
apt-get update
apt-get install -y build-essential linux-headers-$(uname -r)
make clean
make KERNELDIR=/lib/modules/$(uname -r)/build
```

Load:

```sh
insmod ./pccard.ko

# Find the major number assigned to pccard
grep pccard /proc/devices

# Create the device node using the major/minor numbers reported by the driver
mknod /dev/pccard0 c <MAJOR> <MINOR>
chmod 777 /dev/pccard0
```



This was verified on Debian 10.13 with
`4.19.0-21-686-pae`, producing a module with matching vermagic:

```text
4.19.0-21-686-pae SMP mod_unload modversions 686
```

The source includes a compatibility alias for Debian 10's
`PCI_BRIDGE_RESOURCE_NUM` bus-resource API, which is used instead of the older
`PCI_BUS_NUM_RESOURCES` macro.

On Linux 2.6 and later the driver registers `/dev/pccard0` with
`alloc_chrdev_region()` and `cdev_add()`, and the `file_operations` table has
`.owner = THIS_MODULE`. This avoids a Debian 10 / Linux 4.19 Oops in
`chrdev_open()` that occurred before `pccard_open()` was reached.

Debian 10 RF4 runtime library notes:

`/rf4/game/rf4` is a 32-bit executable. On the tested Debian 10.13 i386 install,
`ldd /rf4/game/rf4` only missed the legacy aRts/ESD audio libraries:

```text
libartsc.so.0 => not found
libesd.so.0 => not found
```

Install them with their audiofile dependency and refresh the loader cache:

```sh
sudo apt install libsdl1.2debian:i386 
install -m 0755 ./libartsc.so.0 /usr/local/lib/libartsc.so.0
install -m 0755 ./libesd.so.0 /usr/local/lib/libesd.so.0
install -m 0755 ./libaudiofile.so.0 /usr/local/lib/libaudiofile.so.0
install -m 0755 ./libjpeg.so.62 /usr/local/lib/libjpeg.so.62
ldconfig
ldd /rf4/game/rf4
```

Debian 10 ALSA OSS emulation:

The RF4 binary and old sound plugins expect OSS-style device nodes such as
`/dev/dsp`, `/dev/mixer`, and `/dev/audio`. Debian 10's `4.19.0-21-686-pae`
kernel includes ALSA's OSS compatibility modules:

```sh
modprobe snd-pcm-oss
modprobe snd-mixer-oss
ls -l /dev/dsp /dev/mixer /dev/audio
```

Make the modules load on boot:

```sh
cat >/etc/modules-load.d/oss-alsa-compat.conf <<'EOF'
snd-pcm-oss
snd-mixer-oss
EOF
```

The device nodes are normally owned by `root:audio`. Make sure the user that
launches the game is in the `audio` group, then log out and back in so the group
membership is active:

```sh
usermod -aG audio $USER
```

Linux 2.4 was tested on RHEL 3 and it uses the older external-module compile path and produces
`pccard.o`:

```sh
cd pccard-module
make linux24 KERNELDIR=/usr/src/linux-2.4
insmod ./pccard.o
```
Create the `/dev/pccard0` device node and set its permissions as described
in the Debian 10 instructions above.

The source has compatibility wrappers for the first Linux 2.6 porting step.
It still uses the original direct MMIO access model from the reverse-engineered
driver, so later kernels may need a deeper cleanup around `void __iomem *`,
`ioread8`/`iowrite8`, and device-node creation.

Current Linux 2.6 porting notes:

- The PCI probe accepts TI CardBus bridges by CardBus class plus TI vendor ID,
  instead of only the original `104c:ac50` PCI1410 ID. This keeps the driver
  from missing the bridge if the same hardware transiently reports another TI
  CardBus device ID such as `104c:ac52`.
- Reads use byte-wise MMIO for the 32-bit length field and reread once before
  treating an oversized length as invalid. Direct `*(unsigned int *)` MMIO reads
  were unstable on the newer bridge and could turn valid idle responses into
  false `0xf1` errors.
- Checksum-valid `0xf1` card responses are translated back to the expected game
  command byte after a short settle delay. This matches the observed RF4 idle
  protocol on Linux 2.6.
