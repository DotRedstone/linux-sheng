# Xiaomi Pad 6S Pro (sheng) kernel

This repository carries the Linux kernel device support used by
[`DotRedstone/nixos-sheng`](https://github.com/DotRedstone/nixos-sheng).

## Branches

- `sheng-7.0` is the maintained kernel branch used by current NixOS images.
- `upstream` points to `map220v/sm8550-mainline`, where the original sheng
  mainline enablement is developed.

Device-specific drivers, DTS changes, and temporary fixes live here as normal
Git commits. The NixOS repository pins an exact revision of this branch and
owns the kernel configuration, Mobile NixOS integration, boot image, firmware,
and userspace services.

## Updating the base

Fetch the matching branch from upstream, integrate it without rewriting
published history, then build the `mobileAndroidBootimg` output in
`nixos-sheng`. Hardware fixes should remain separate commits so regressions can
be bisected and patches can be submitted upstream independently.

Linux kernel source and local modifications are licensed under GPL-2.0-only;
see [`COPYING`](COPYING).
