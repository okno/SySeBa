#!/bin/sh
set -e

if command -v systemctl >/dev/null 2>&1 &&
    systemctl is-active --quiet syseba.service; then
    install -d -m 0750 /var/lib/syseba
    printf '1\n' >/var/lib/syseba/.service-was-active
    systemctl stop syseba.service
fi
exit 0
