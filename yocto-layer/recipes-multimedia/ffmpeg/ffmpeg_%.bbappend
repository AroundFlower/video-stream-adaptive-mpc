# Strip external codec packages — mpeg2video/rawvideo/mpegts are built-in,
# so removing external deps shrinks the ffmpeg binary without losing functionality.
PACKAGECONFIG_remove = " \
    x11 \
    x264 \
    x265 \
    vpx \
    mp3lame \
    schroedinger \
    theora \
    vorbis \
    speex \
    libv4l \
    openssl \
"
