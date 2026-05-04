SUMMARY = "UDP-TCP bridge for ADB forwarding (device side)"
LICENSE = "CLOSED"

inherit systemd

SRC_URI = "file://bridge.py \
           file://device-bridge.service"

S = "${WORKDIR}"

do_compile[noexec] = "1"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 bridge.py ${D}${bindir}

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/device-bridge.service ${D}${systemd_system_unitdir}
}

RDEPENDS_${PN} = "python3-core"

SYSTEMD_SERVICE_${PN} = "device-bridge.service"
SYSTEMD_AUTO_ENABLE = "enable"
