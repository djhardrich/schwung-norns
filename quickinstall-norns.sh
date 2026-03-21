#!/usr/bin/env bash
set -euo pipefail

mkdir me-norns-temp && cd me-norns-temp

git clone https://github.com/djhardrich/move-everything-norns
git clone https://github.com/djhardrich/move-everything-pipewire

mkdir -p move-everything-norns/dist move-everything-pipewire/dist

curl -L -o move-everything-norns/dist/norns-module.tar.gz https://github.com/djhardrich/move-everything-norns/releases/download/v0.2.0/norns-module.tar.gz
curl -L -o move-everything-norns/dist/norns-move-prebuilt.tar.gz https://github.com/djhardrich/move-everything-norns/releases/download/v0.2.0/norns-move-prebuilt.tar.gz
curl -L -o move-everything-pipewire/dist/pipewire-module.tar.gz https://github.com/djhardrich/move-everything-pipewire/releases/download/v0.2.0/pipewire-module.tar.gz
curl -L -o move-everything-pipewire/dist/pw-chroot-desktop.tar.gz https://github.com/djhardrich/move-everything-pipewire/releases/download/v0.2.0/pw-chroot-desktop.tar.gz

cd move-everything-pipewire && ./scripts/install.sh
cd ../move-everything-norns && ./scripts/install.sh

ssh root@move.local 'sh /data/setup-norns.sh'
