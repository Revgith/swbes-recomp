# Third-party material

## psprecomp

The recompiler and PSP runtime in `psprecomp/`. MIT licence, see
`psprecomp/LICENSE` and `psprecomp/CONTRIBUTORS.md`.

## Dear ImGui

The PC menu (`psprecomp/third_party/imgui`). Copyright (c) 2014-2025 Omar
Cornut. MIT licence, see `psprecomp/third_party/imgui/LICENSE.txt`.

## ATRAC3 / ATRAC3+ decoder

Game audio (`psprecomp/third_party/at3_standalone`), derived from FFmpeg.
Copyright Maxim Poliakovski, Benjamin Larsson and the FFmpeg authors. GNU
Lesser General Public License 2.1; see the headers of its source files.

## ebootdec

`tools/ebootdec`, a separate program that decrypts the player's EBOOT on first
start. Built from pspdecrypt (https://github.com/John-K/pspdecrypt) and
PPSSPP's PrxDecrypter and libkirk. GNU General Public License 3.0, see
`tools/ebootdec/LICENSE.TXT`. Releases include its source.

## FFmpeg

Movie playback runs an `ffmpeg.exe` found beside `swbes.exe` or on PATH.
`scripts/build_ffmpeg.sh` builds a minimal LGPL one (FFmpeg 7.1), which releases
include with its licence; its source is published beside each release.

## IconPark: `enter-key` (Solid)

Used for the Enter key prompt. Rasterized into `games/swbes/icon_enter_key.h`.

Source: https://github.com/bytedance/IconPark. Copyright ByteDance. Licensed
under the Apache License, Version 2.0: http://www.apache.org/licenses/LICENSE-2.0.
Change made: the SVG was converted to a 56x56 coverage table.

## UIX Icons: `mouse-scroll` (Solid)

Used for the mouse-wheel button prompt. Rasterized into
`games/swbes/icon_mouse_scroll.h`.

Source: https://github.com/mdrakibshekh/uix-icons

```
MIT License

Copyright (c) 2026 Rakib Shekh, trading as Sigma Studio

Permission is hereby granted, free of charge, to any person obtaining a copy
of the Free Styles and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
