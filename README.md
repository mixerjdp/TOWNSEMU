# Tsugaru libretro

Tsugaru libretro is a fork of **Tsugaru**, the FM Towns / FM Towns Marty emulator by CaptainYS, adapted to run as a **RetroArch / libretro** core.

The goal of this fork is to provide a fast, stable, and easy-to-install FM Towns core for RetroArch, while keeping the Tsugaru backend as the foundation and avoiding redistribution of proprietary BIOS or ROM files.

## Core features

- FM Towns / FM Towns Marty libretro core.
- CD media support, including `CHD`.
- Support for floppy and disk images by extension.
- RetroArch `.info` metadata file.
- 32-bit and 64-bit build scripts.
- No BIOS redistribution in the release.

## What is included

- The libretro core binaries.
- The `.info` file for RetroArch.
- Build scripts for Win32 and Win64.

## What is not included

- System BIOS or ROM images.
- Commercial game content.
- Third-party software packages that cannot be redistributed.

## Requirements

To use the core you need:

- RetroArch installed.
- The `tsugaru_libretro.dll` core for your architecture.
- The `tsugaru_libretro.info` file.
- Legal FM Towns BIOS / ROM files.

## Install the core

### 64-bit

Copy:

```text
tsugaru_libretro.dll
```

into your RetroArch cores directory, for example:

```text
RetroArch/cores/
```

### 32-bit

If you use a 32-bit RetroArch installation, copy the Win32 build of the same core into that installation's `cores` directory.

## Install the `.info` file

Copy:

```text
tsugaru_libretro.info
```

into your RetroArch info directory, for example:

```text
RetroArch/info/
```

RetroArch uses this file to show the core name and to recognize the supported content extensions.

## Install BIOS and ROM files

Place the system files in RetroArch's `system` directory. The recommended layout is:

```text
RetroArch/system/fmtowns/
```

If your setup does not use a `fmtowns` subfolder, the core can also fall back to the top-level `system` directory.

Typical BIOS / ROM filenames include:

```text
FMT_SYS.ROM
FMT_DOS.ROM
FMT_FNT.ROM
FMT_DIC.ROM
```

Exact filenames can vary depending on the ROM set you use, but the rule is the same: keep firmware under `system`, not under `cores`.

## Supported content

The core accepts FM Towns content by file extension. Current supported extensions are:

```text
cue, bin, iso, mds, mdf, ccd, chd, d77, d88, rdd, img, fdi, hdm, h0, m3u
```

Notes:

- CD media support includes `cue`, `bin`, `iso`, `mds`, `mdf`, `ccd`, and `chd`.
- Floppy and disk image support includes `d77`, `d88`, `rdd`, `img`, `fdi`, `hdm`, and `h0`.
- `m3u` can be used for multi-disc content lists when supported by your RetroArch workflow.

## How to run

1. Start RetroArch.
2. Load the `Tsugaru` core.
3. Load a supported FM Towns image.
4. If the game needs BIOS files, confirm they are placed under `RetroArch/system/fmtowns/`.

## Local build

Build scripts are provided for both architectures:

```bat
buildlibretro32.bat
buildlibretro64.bat
```

The 64-bit script copies the resulting DLL into the RetroArch `cores` directory.

## Release v1.00

The `v1.00` release publishes only these files:

- `tsugaru_libretro_32bit.dll`
- `tsugaru_libretro_64bit.dll`
- `tsugaru_libretro.info`

No BIOS files are included in the release.

## Thanks

- CaptainYS and the Tsugaru contributors.
- The libretro community for the documentation and the wider core ecosystem.

