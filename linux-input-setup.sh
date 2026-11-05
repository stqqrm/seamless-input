#!/usr/bin/env bash
# One-time setup for keyboard/mouse sharing on Wayland (also works for X11).
# The app talks to the kernel directly: it reads /dev/input/event* (to see your mouse
# and keyboard while it forwards them) and writes /dev/uinput (to act as a virtual
# keyboard/mouse when another PC controls this one).
#
# SECURITY: membership of the 'input' group lets a program read every keystroke on this
# machine. Only do this on a PC you trust, with software you trust.
set -euo pipefail

[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"
TARGET_USER="${SUDO_USER:-$USER}"

echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"' \
    > /etc/udev/rules.d/60-seamless-input.rules
echo uinput > /etc/modules-load.d/seamless-input.conf
modprobe uinput
getent group input >/dev/null || groupadd input
usermod -aG input "${TARGET_USER}"
udevadm control --reload-rules
udevadm trigger --name-match=uinput

echo "Done. Log out and back in so '${TARGET_USER}' picks up the 'input' group."
