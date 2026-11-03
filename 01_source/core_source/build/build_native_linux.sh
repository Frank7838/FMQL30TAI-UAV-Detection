#!/usr/bin/env bash
# 编译CPU取样与MPEG4编码库。
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output_root="$source_root/build_output/native"
mkdir -p "$output_root"
compiler=${CC:-gcc}
"$compiler" -std=c11 -O2 -fPIC -shared -Wall -Wextra -Werror \
  "$source_root/src/board/native/center_pack_m93.c" -o "$output_root/libcenter_pack_m93.so"

# Codec build is opt-in and uses exact supplied libraries, without searching a
# possibly incompatible system FFmpeg. Header provenance is documented separately.
if [[ ${BUILD_CODEC:-0} == 1 ]]; then
  : "${FFMPEG_HEADERS:?Point to matching FFmpeg 4.4.4 headers}"
  : "${AVCODEC_LIBRARY:?Specify full existing shared-library path}"
  : "${AVFORMAT_LIBRARY:?Specify full existing shared-library path}"
  : "${AVUTIL_LIBRARY:?Specify full existing shared-library path}"
  : "${SWSCALE_LIBRARY:?Specify full existing shared-library path}"
  library_root=$(dirname -- "$AVCODEC_LIBRARY")
  "$compiler" -std=gnu11 -O2 -fPIC -shared -Wall -Wextra -Werror -I"$FFMPEG_HEADERS" \
    "$source_root/src/board/native/mpeg4_writer_m388.c" \
    "$AVCODEC_LIBRARY" "$AVFORMAT_LIBRARY" "$AVUTIL_LIBRARY" "$SWSCALE_LIBRARY" \
    -Wl,-rpath,"$library_root" -Wl,-rpath-link,"$library_root" -Wl,--disable-new-dtags \
    -o "$output_root/libmpeg4_writer_m388.so"

  # Timestamp variant: same pixel conversion, millisecond VFR presentation times.
  "$compiler" -std=gnu11 -O2 -fPIC -shared -Wall -Wextra -Werror -I"$FFMPEG_HEADERS" \
    "$source_root/src/board/native/mpeg4_writer_timed.c" \
    "$AVCODEC_LIBRARY" "$AVFORMAT_LIBRARY" "$AVUTIL_LIBRARY" "$SWSCALE_LIBRARY" \
    -Wl,-rpath,"$library_root" -Wl,-rpath-link,"$library_root" -Wl,--disable-new-dtags \
    -o "$output_root/libmpeg4_writer_timed.so"

fi
