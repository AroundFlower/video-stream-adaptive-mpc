SUMMARY = "Video stream transmitter image (imx6ull, ~100MB)"
LICENSE = "MIT"

inherit core-image

IMAGE_INSTALL += " \
    ffmpeg-sender \
    device-bridge \
    ffmpeg \
    python3-core \
"

# Target rootfs size ~100 MB (ext4 image)
IMAGE_ROOTFS_SIZE = "102400"
IMAGE_ROOTFS_EXTRA_SPACE = "0"

# Remove unnecessary features to save space
IMAGE_FEATURES_remove = " \
    splash \
    hwcodecs \
    package-management \
"
