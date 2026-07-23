#!/bin/sh
set -e

install -d -m 0750 /etc/syseba /var/lib/syseba /var/log/syseba /run/syseba
install -d -m 0750 /srv/syseba/source /srv/syseba/backup /srv/syseba/restore

if [ -f /opt/syseba/syseba.py ] &&
    [ ! -f /var/lib/syseba/.migrated-from-opt ]; then
    if [ -f /opt/syseba/syseba.conf ] &&
        cmp -s /etc/syseba/syseba.conf /usr/share/syseba/syseba.conf.default; then
        install -m 0640 /opt/syseba/syseba.conf /etc/syseba/syseba.conf
    fi
    if [ ! -f /var/lib/syseba/syseba_logs.db ] &&
        [ -f /opt/syseba/syseba_logs.db ]; then
        cp -p /opt/syseba/syseba_logs.db* /var/lib/syseba/ 2>/dev/null || true
    fi
    if [ ! -f /etc/syseba/syseba_web.token ] &&
        [ -s /opt/syseba/syseba_web.token ]; then
        install -m 0600 \
            /opt/syseba/syseba_web.token \
            /etc/syseba/syseba_web.token
    fi
    if [ -f /etc/systemd/system/syseba.service ] &&
        grep -q '/opt/syseba/syseba.py' \
            /etc/systemd/system/syseba.service; then
        cp -p \
            /etc/systemd/system/syseba.service \
            /var/lib/syseba/legacy-syseba.service
        rm -f /etc/systemd/system/syseba.service
    fi
    date -u +%Y-%m-%dT%H:%M:%SZ >/var/lib/syseba/.migrated-from-opt
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    systemctl enable syseba.service >/dev/null 2>&1 || true
    if /usr/bin/syseba config-check \
        --config /etc/syseba/syseba.conf >/dev/null 2>&1; then
        systemctl restart syseba.service || true
    fi
fi
rm -f /var/lib/syseba/.service-was-active
exit 0
