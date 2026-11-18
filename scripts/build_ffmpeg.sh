# Minimal LGPL-2.1 ffmpeg (H.264 decode -> raw BGRA) for movie playback.
# Needs MSYS2 (mingw-w64-x86_64-gcc make nasm diffutils pkgconf); source: ffmpeg.org/releases/ffmpeg-7.1.tar.xz into third_party/ffmpeg-src.
# Shipping it: scripts/package.ps1 adds the LGPL-2.1 text, this script and the source.
# The prefix is a neutral /usr/local (ffmpeg embeds its configure line); install goes to third_party/ffmpeg-min.
export PATH=/mingw64/bin:/usr/bin:$PATH
R=$(cd "$(dirname "$0")/.." && pwd); cd "$R/third_party/ffmpeg-src/ffmpeg-7.1"
./configure --prefix=/usr/local --disable-everything --disable-autodetect --disable-doc --disable-network --disable-debug --disable-ffplay --disable-ffprobe --enable-small --enable-static --disable-shared --extra-ldflags=-static \
 --enable-decoder=h264 --enable-demuxer=h264 --enable-parser=h264 --enable-protocol=file,pipe --enable-muxer=rawvideo --enable-encoder=rawvideo --enable-filter=scale,format,null,aformat,anull,atrim,trim,copy,buffer,buffersink,abuffer,abuffersink --enable-swscale && make -j8 && make install prefix="$R/third_party/ffmpeg-min" && strip "$R/third_party/ffmpeg-min/bin/ffmpeg.exe"
