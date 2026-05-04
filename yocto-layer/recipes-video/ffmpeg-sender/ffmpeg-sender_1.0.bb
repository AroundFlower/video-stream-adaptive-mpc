SUMMARY = "Video stream sender — native V4L2 capture + pipe fork ffmpeg + MPC pacing"
LICENSE = "CLOSED"

SRC_URI = "file://ffmpeg_sender.cpp \
           file://mpc_controller.hpp \
           file://v4l2_capture.hpp"

S = "${WORKDIR}"

do_compile() {
    ${CXX} -std=c++11 -O2 -Wall -DUSE_MPC \
        ${S}/ffmpeg_sender.cpp \
        -o ffmpeg_sender \
        -lpthread
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ffmpeg_sender ${D}${bindir}
}

# ffmpeg is a runtime dependency (external binary, called via pipe+fork)
RDEPENDS_${PN} = "ffmpeg"
