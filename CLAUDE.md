# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build everything (requires kernel headers installed)
make all

# Build only the userspace app (no kernel headers needed)
make app

# Build only kernel modules
make drivers

# Override kernel source directory
make drivers KDIR=/path/to/kernel/source

# Clean all build artifacts
make clean
```

## Kernel Module Lifecycle

```bash
# Install prerequisites (Ubuntu/Debian)
sudo apt-get install build-essential linux-headers-$(uname -r)

# Verify DES crypto is available in the running kernel
grep CONFIG_CRYPTO_DES /boot/config-$(uname -r)   # need =y or =m

# Load DES dependency (only if CONFIG_CRYPTO_DES=m)
sudo modprobe des_generic

# Load the crypto driver (creates /dev/ltcrypt via udev)
sudo insmod driver/ltcrypt/ltcrypt.ko
# or:
make load

# If /dev/ltcrypt was not auto-created by udev:
make mknod

# Check driver loaded correctly
dmesg | tail -5   # expect: "ltcrypt: loaded, major=XXX"

# Unload all drivers
make unload
# or manually:
sudo rmmod ltcrypt
```

## USB Mouse Driver Testing

The generic `usbhid` driver claims mice before `ltusbmouse`. Manual rebind is required:

```bash
sudo insmod driver/usbmouse/ltusbmouse.ko

# Find the device ID from lsusb, then:
echo "BUSNUM-PORTNUM:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/unbind
echo "BUSNUM-PORTNUM:1.0" | sudo tee /sys/bus/usb/drivers/ltusbmouse/bind

# Watch live events (move mouse to generate logs)
sudo dmesg -w   # expect: "ltusbmouse: btn=00 X=5 Y=-3"
```

## WSL2 Note

WSL2 kernels don't ship build headers by default. Drivers cannot be built against the running WSL2 kernel without compiling a custom WSL2 kernel. The userspace app (`make app`) always builds without this dependency.

## Architecture

### IPC / ABI Boundary

`include/ltcrypt.h` is the **single shared header** used by both the kernel driver and the userspace app. It defines the ioctl numbers and `struct ltcrypt_data`. Do not define these values in two places.

### Crypto Driver (`driver/ltcrypt/ltcrypt.c`)

Single global `struct ltcrypt_dev` (one minor device). All operations are mutex-protected. Two interaction modes:

1. **ioctl** — preferred by `ltfm`: `SET_KEY` → `ENCRYPT` / `DECRYPT` (in-place on `struct ltcrypt_data.data`, updates `.len` to padded length)
2. **write→read pipe** — `write()` encrypts plaintext into an internal `rd_buf`; subsequent `read()` returns ciphertext. Stateful and not safe to interleave with ioctl.

DES operates in ECB mode (one block at a time via `crypto_cipher_encrypt_one` / `crypto_cipher_decrypt_one`). No IV, no chaining.

### USB Mouse Driver (`driver/usbmouse/ltusbmouse.c`)

Standard Linux USB + input subsystem pattern:
- `probe()` finds the interrupt-IN endpoint, allocates a DMA-coherent buffer and a single URB, registers an `input_dev`, then submits the URB.
- `ltmouse_irq()` (runs in interrupt context) parses the 3-byte HID Boot Mouse report, reports events, and immediately resubmits the URB with `GFP_ATOMIC`.
- `disconnect()` kills the URB before freeing resources (order matters to avoid use-after-free).

### Userspace App (`app/ltfm.c`)

Encrypted file format:
```
[ciphertext — padded to multiple of 8 bytes] [4-byte LE uint32: original file size]
```
The footer is written by `encrypt_file()` and read first by `decrypt_file()` to trim zero-padding from the last block. Files are named `<original>.enc`.

The app opens `/dev/ltcrypt` lazily on first operation that needs it. Key is set once per session via ioctl and mirrored in `g_key[]` for display; the kernel driver holds the authoritative key in the cipher handle.
