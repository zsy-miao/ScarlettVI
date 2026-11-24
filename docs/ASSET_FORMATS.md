# RPF9 and shader-container research

## Scope and method

ScarlettVI's `archives` command inventories **only the first 64 bytes** of each
RPF9 archive header. It neither extracts the payload nor reads, decrypts, or
copies any archive table of contents. It is a completely independent C++20
implementation grounded in the supplied files' magic bytes and layout.

All observations below are from read-only inspection of the user's supplied
Scarlett-targeted installation. The archive structure is still partly unknown.

## Observed RPF9 header

The first four bytes are `39 46 50 52` (ASCII `9FPR`; LE integer
`0x52504639`). The first 0x40 bytes hold at least:

| Offset | Type | Observed meaning | Confidence |
| --- | --- | --- | --- |
| 0x00 | u32 LE | RPF9 magic | Directly verified |
| 0x04 | u32 LE | End offset of first table of contents | Corroborated by public research |
| 0x08 | u32 LE | Variant-dependent field | Unknown |
| 0x0C | u32 LE | Length field | Meaning incomplete |
| 0x24 | u32 LE | First header entry count | Corroborated by public research |
| 0x2C | u32 LE | Header index variant | Directly verified |

Do not treat the header's entry count as the total count of assets/shaders.
Archives can contain nested RPF9 archives; the `shaders.rpf` outer header
contains 3 entries, **not** 3 shader programs. Nested archives are not
currently enumerated by ScarlettVI.

## Observed top-level archives

| Archive | Size in MiB (approx.) | TOC end | Header entries | Variant |
| --- | ---: | ---: | ---: | --- |
| anim.rpf | 4,203.4 | 0x4660 | 265 | 0x90F02 |
| appdata0.rpf | 0.0 | 0x360 | 2 | 0x90F01 |
| audio.rpf | 27,221.2 | 0x1350 | 103 | 0x90F01 |
| common.rpf | 6.3 | 0x9580 | 418 | 0x90F02 |
| data.rpf | 459.2 | 0x11060 | 699 | 0x90F02 |
| levels.rpf | 72,627.1 | 0x1B070 | 2078 | 0x90F02 |
| movies.rpf | 8,423.3 | 0x6AC0 | 515 | 0x90F02 |
| navmeshes.rpf | 6,396.8 | 0x780 | 29 | 0x90F01 |
| packs.rpf | 4,575.9 | 0x4550 | 172 | 0x90F01 |
| script.rpf | 4.2 | 0x260 | 2 | 0x90F01 |
| shaders.rpf | 392.6 | 0x8640 | 3 | 0x90F01 |
| textures.rpf | 183.7 | 0x790 | 22 | 0x90F01 |

Observed 12 top-level archives, 130,541,188,608 total bytes and 4,308
**outer** header entries. The archive files themselves have never been
modified by ScarlettVI.

## Graphics research priorities

1. Establish a read-only asset stream interface (bounded file offsets,
   resource ownership and 64 KiB block boundaries), without depending on a
   decrypted archive directory.
2. Identify shader containers and native RDNA2 machine instructions only
   from permitted, user-owned inputs. Distinguish DXBC wrappers from actual
   shader instruction data, and separate shader metadata from shader code.
3. Implement original, independently specified console GPU resource layout,
   texture tiling, descriptor translation and shader-to-DXIL transformation.
4. Validate against generated standalone test assets before binding title
   shaders. No proprietary GPU code or extracted game data belongs on GitHub.

Public architectural reading only (no copied code):
- [TrinitronClient — RPF9 byte-level structure observations](https://github.com/TrinitronClient/GTA6-Asset-Info/blob/main/structure/01-rpf9-container.md)
- [0x1F9F1 Swage — overview of RPF family format identifiers](https://github.com/0x1F9F1/Swage/blob/master/src/games/rage/rpf.md)

The public observations do not constitute decrypted archive access,
a working resource streamer, or a running shader compiler.
