# Camera image pipeline (userland)

Raw sensor dumps from the sapphire cameras -> DNG / JPEG / PNG / PPM. Plain C11 + pthreads,
no external libraries; the same code builds for the PC (WSL, for tests with synthetic or real
dumps) and for the phone (arm64 iOS). Background on the sensors: `research/camera.md`.

| file | what |
|---|---|
| `rawtool.c` | the CLI: options, presets, pipeline graph, `detect`, `psnr` |
| `stage.c/h` | row streaming: every stage keeps a ring of rows, chunks run on a thread pool |
| `rawio.c` | input: MIPI RAW10/RAW12 packed, RAW8, RAW16, PGM; any stride/offset; gray-world stats; layout detection |
| `remosaic.c` | nonacell (3x3) / tetracell (2x2) -> RGB -> Bayer; cell binning for the preview |
| `demosaic.c` | Bayer demosaic: bilinear, Malvar-He-Cutler |
| `color.c` | denoise (light, edge-aware), WB clip + CCM + exposure, sharpen, box downscale |
| `out.c`, `jpeg.c` | writers: DNG 1.4 (16-bit CFA), baseline JPEG (own encoder), PNG, PPM, PGM |
| `rawsynth.c`, `imgread.c` | test images and a sensor simulator (reads PNG/PPM, writes RAW10/RAW16 dumps) |
| `tests/run.sh`, `tests/bench.sh`, `tests/dngcheck.py` | round-trip tests (PSNR), full-size benchmark, DNG parser check |

## Build

```sh
make            # build/host/{rawtool,rawsynth}  (clang 18, -O3 -march=native)
make ios        # build/ios/{rawtool,rawsynth}   (arm64-apple-ios15.0, iPhoneOS16.5.sdk, lld, ldid -S)
make test       # tests/run.sh
```

## What the capture side must provide

| parameter | option | S5KHM6 values |
|---|---|---|
| size | `--size WxH` | mode 0: 12000x9000, mode 1: 4000x3000, mode 4: 4000x3000 |
| format | `--fmt` | `raw10` = MIPI RAW10 (DT 0x2B), 4 pixels in 5 bytes |
| line stride in bytes | `--stride` | what the TFE write master was programmed with (e.g. 15008 = 15000 padded to 32). If omitted it is taken from the file size (size / height) or the unpadded 15000 |
| header bytes | `--offset` | 0 for a bare frame |
| black / white level | `--black`, `--white` | 64 / 1023 (CamX module data: 0x40 for R/Gr/Gb/B, white 0x3FF; SC202PCS black 0x41) |
| CFA order of the blocks | `--cfa` | `grbg` (see below) |
| cell size | `--cell` | mode 0 and 4: 3 (nonacell); binned modes: 1 |
| tile phase | `--phase X,Y` | 0,0 when the frame starts on a 6x6 tile; mode 4 starts at x_addr 3996 / y 3000 = tile aligned, so 0,0 unless a digital crop shifts it |

`--preset hm6 | hm6-bin | hm6-crop | 4h7 | ov16a1q | gc16b3c | sc202` sets size, cell, CFA,
black and white for each sensor mode; options after it override.

CFA order (**verify**): every CamX sensor module of this phone (HM6, 4H7, OV16A1Q, GC16B3C,
SC202PCS) has `colorFilterArrangement` = 2, which is `BAYER_GRBG` in CamX's enum (BGGR 0,
GBRG 1, GRBG 2, RGGB 3). Mirror/flip registers (`0x0101`, OV `0x3820/0x3821`) move it. To check
a real dump: `rawtool detect` finds the cell size, the tile phase and which diagonal has the
greens; R vs B needs a red or blue object in the frame (wrong guess = red and blue swapped).
The front sensors only run binned (plain Bayer) modes in these tables; `--cell 2` is there
for a tetracell full-resolution mode.

## Commands

```sh
# 108 MP still: DNG (remosaiced RGGB Bayer) + JPEG + small preview, auto WB
rawtool --preset hm6 --stride 15008 --dng out.dng --jpeg out.jpg --preview prev.jpg frame.raw

# preview only (3x3 cell binning = the binned Bayer image, 4000x3000 JPEG; 0.4 s on the PC)
rawtool --preset hm6 --preview prev.jpg --preview-scale 2 frame.raw

# mode 1 (binned Bayer) or the other sensors
rawtool --preset hm6-bin --jpeg out.jpg --dng out.dng frame.raw
rawtool --preset 4h7 --wb 1.9,1,1.6 --ccm typical --sharpen 0.4 --jpeg out.jpg frame.raw

# mode 4 crop (4000x3000 of the nonacell pattern), explicit layout
rawtool --size 4000x3000 --cell 3 --cfa grbg --phase 0,0 --black 64 --jpeg crop.jpg frame.raw

# inspect a dump: layout detection, 16-bit PGM of the raw values, PNG for viewing
rawtool detect --size 12000x9000 frame.raw
rawtool --preset hm6 --unpack raw16.pgm frame.raw
rawtool --preset hm6 --png view.png --bits 16 --scale 3 frame.raw
```

Processing options: `--wb auto|R,G,B` (auto = gray world over unclipped pixels), `--ccm
identity|typical|9 numbers` (camera RGB -> linear sRGB; `typical` is a generic phone-sensor
matrix until the module is calibrated), `--exposure EV`, `--gamma srgb|linear|2.2`,
`--denoise S`, `--sharpen A`, `--demosaic mhc|bilinear`, `--via-bayer` (nonacell: remosaic to
Bayer and demosaic that, instead of using the remosaic's RGB directly), `--out-cfa` (Bayer
order written to the DNG, default rggb), `--scale K`, `--quality Q`, `--threads N`,
`--chunk ROWS`, `-v`. Remosaic tuning: `--remosaic guided|simple`, `--rm-iter N` (default 1,
0 = faster), `--rm-blend`, `--rm-sigma`, `--rm-eps`.

The DNG has the raw values (white balance divided out again) as 16-bit with BlackLevel =
black x 64 and WhiteLevel = white x 64, CFAPattern of `--out-cfa`, AsShotNeutral from the WB
gains, ColorMatrix1 = inverse(CCM) x XYZ->sRGB (D65), i.e. "the camera is sRGB after the
CCM": an approximation until there is a colour calibration. Values below black are kept.

## Remosaic algorithm (nonacell / tetracell -> Bayer)

The nonacell pattern is a Bayer pattern of 3x3 same-colour blocks (6x6 tile). Each R/B block
has green blocks on its four sides; each green block has R blocks on one axis and B on the
other.

1. **Green in R/B blocks** (`gest`): along each axis, the colour difference D = G - C is
   measured just outside both block edges (half-pixel slope corrected, so a linear ramp is
   exact) and interpolated across the block: G = C + D(x). C is sampled at every pixel of the
   block, so luminance detail inside the block is kept (grey edges come out exact). Horizontal
   and vertical estimates are blended by 1/(eps + activity)^2 (change of D across the block,
   green gradients next to it, own-colour change).
2. **R/B everywhere** (`remosaic`): K = C - G is gathered from the nearest pixel of each C
   block within two blocks, weighted by 1/d^2 and by green similarity 1/(1 + (dG/sigma)^2)
   (joint-bilateral / guided interpolation, keeps colour from bleeding over edges).
3. **Refinement** (`refine-g`, `refine-rb`, `--rm-iter 1`): green inside R/B blocks again, now
   gathering D = G - C_est from the facing lines of the side green blocks and the nearest pixels
   of the others, weighted by similarity of the block's own (true) colour C; blended 60/40 with
   step 1; then step 2 again.
4. **Bayer** (`bayer`): the channel the output CFA wants at each pixel. JPEG/PNG use the RGB
   of step 3 directly (better than going through Bayer and demosaicing again).

## Memory and streaming

Nothing holds a full frame. Each stage keeps a ring of rows (sized from the longest
downstream path), rows are computed in chunks of `--chunk` rows split over the threads, and
all writers advance in lockstep. A 108 MP frame (RAW10 135 MB, Bayer16 216 MB, RGB16 648 MB)
runs with:

| run (12000x9000, DNG + JPEG + preview) | rings | peak RSS |
|---|---|---|
| `--chunk 64` (default) | 229 MB | 239 MB |
| `--chunk 32` | 131 MB | 139 MB |
| `--chunk 16` | 83 MB | 89 MB |
| preview only | 68 MB | 74 MB |
| PNG with `--scale 3` (downscaling multiplies the ring depth upstream) | 343 MB | 350 MB |

## Test results (PC: WSL, 16 threads)

`sh tests/run.sh [photos...]`: rawsynth turns an image into a sensor dump (sRGB -> linear,
channel sensitivity 0.55/1/0.7, cell pattern, RAW10), rawtool processes it back with the
matching WB and identity CCM, PSNR against the source (16-bit, 8-pixel border ignored):

| image | nonacell simple | **nonacell guided** | via Bayer | remosaic Bayer vs ideal | true Bayer + MHC | true Bayer bilinear |
|---|---|---|---|---|---|---|
| natural (synthetic) | 37.90 | **39.89** | 36.01 | 48.09 | 37.02 | 40.24 |
| siemens star | 30.44 | **38.06** | 35.58 | 42.40 | 37.63 | 35.28 |
| colour bars | 19.40 | **29.15** | 21.91 | 32.71 | 22.49 | 20.30 |
| hue gradient | 58.90 | **49.54** | 49.13 | 64.26 | 56.93 | 59.66 |
| text (1-6 px strokes) | 12.75 | **17.75** | 14.95 | 21.65 | 16.64 | 16.01 |
| zone plate to 0.35 cy/px | 11.58 | **17.47** | 15.01 | 18.93 | 17.53 | 12.69 |
| photo 1 (1920x1200) | 30.48 | **32.43** | 31.07 | 36.11 | 33.38 | 33.73 |
| photo 2 | 36.00 | **37.93** | 36.36 | 42.71 | 38.65 | 37.76 |
| photo 3 | 34.97 | **37.41** | 34.22 | 41.69 | 36.14 | 38.96 |
| photo 4 | 38.67 | **49.25** | 44.64 | 51.90 | 45.44 | 42.74 |

(photos: Windows wallpapers downscaled to 1920x1200, i.e. sharp content.) The guided
remosaic of a nonacell sensor ends up about level with a real Bayer sensor of the same pixel
count demosaiced with MHC, and 2-10 dB above the naive per-plane interpolation. Other checks:
tetracell 41.0 dB, tile phase 4,1 with 64-byte stride 38.7 dB, RAW16 input, noise (read 2 DN +
shot) 33.1 dB -> 34.6 dB with `--denoise 1`, JPEG decoded by ffmpeg 35-40 dB vs the lossless
output at quality 92, DNG read by ffprobe (`bayer_rggb16le`) and by `tests/dngcheck.py` (tags,
strips, pixels = the `--bayer` dump within 1 code).

Speed, 12000x9000 (`sh tests/bench.sh`, 16 threads): DNG + JPEG + preview 2.3-4.0 s (depends
on JPEG content), JPEG only with `--rm-iter 0` 2.0 s, preview only 0.4 s; single thread 11.3 s.
Per stage (16 threads): gest 0.35 s, remosaic 0.65, refine-g 0.63, refine-rb 0.64, DNG write
0.26, JPEG 0.5-0.65. The phone (4x A73 + 4x A53) is not measured yet: run `tests/bench.sh`
there with `BIN=` pointing at the iOS binaries.

## Not done yet

Lens shading and per-module colour calibration (module EEPROM), defect/PDAF pixel masking
(if mode 0 embeds PD pixels in the image they will show as dots), highlight reconstruction
(clipped areas go white), HEIC output, EXIF in the JPEG.

## camd: the camera daemon (`camd/`)

The phone side of all this: a root daemon (launchd job `com.sapphire.camd`, `/redwood/bin/camd`)
that owns `kern.sweet_cam`, streams the preview to apps and develops stills with this library
linked in (a forked child per still). Protocol and shared-memory layout: `camd/camd_proto.h`
(copied into the SapphireCamera app as `SCCamdProtocol.h`).

| file | what |
|---|---|
| `camd.c` | socket `/var/run/camd.sock`, preview loop, AE/AWB, stills, idle power-off, `/var/root/camd.conf` |
| `camk.c` | the kernel ops: 0276 (preview ring op 30/31, still op 32, `kern.sweet_cam_exp`, `_latest`, `_sel`) or 0275 (stream + op 23 per frame, exposure over `kern.sweet_cam_i2c`), picked at start by which sysctls exist |
| `preview.c` | RAW10 -> upright BGRX (block means, or 2x2 quads at full size for the 1280x720 mode) + metering/gray-world statistics in one pass |
| `develop.c` | rawtool's graph for camd: JPEG (+ DNG), WB from the preview, brightness check against the preview |
| `exif.c` | the JPEG's Exif (orientation, date, exposure); written through `g_jpeg_app1` in `jpeg.c` |
| `camctl.c` | `camctl STATUS`, `camctl -s 20 START`, `camctl CAPTURE 108 dng`, `camctl SET rotate 270` |

```sh
bash camd/build.sh test     # preview + develop on rawsynth frames (PC)
bash camd/install.sh        # build, install on the phone (launchd cache, backups), restart camd
bash camd/uninstall.sh      # remove (or --rollback to the previous binaries)
```

Stills: 108 MP = op 32 (0276) or mode 0 + op 23 (0275), the raw (135 MB) to `/var/tmp`, developed
into Files' "On My iPhone"/Camara/`IMG_YYYYMMDD_HHMMSS.jpg` (+ `.dng` with `SET dng 1` or
`CAPTURE 108 dng`), a thumbnail from the preview in `Camara/.thumbs/`. 12 MP = one mode 1 frame.
The sensor's image is landscape: the preview is rotated and the JPEG gets an Exif orientation
from `rotate` (default 90 = clockwise; `camctl SET rotate 270` if the photos come out upside
down). Log: `/var/tmp/camd.log`.
