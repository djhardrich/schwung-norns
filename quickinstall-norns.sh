#!/usr/bin/env bash
set -euo pipefail

mkdir schwung-norns-temp && cd schwung-norns-temp

git clone https://github.com/djhardrich/schwung-norns
git clone https://github.com/djhardrich/schwung-chroot-linux

mkdir -p schwung-norns/dist schwung-chroot-linux/dist

curl -L -o schwung-norns/dist/norns-module.tar.gz https://github.com/djhardrich/schwung-norns/releases/download/v0.4.0/norns-module.tar.gz
curl -L -o schwung-norns/dist/norns-move-prebuilt.tar.gz https://github.com/djhardrich/schwung-norns/releases/download/v0.4.0/norns-move-prebuilt.tar.gz
curl -L -o schwung-chroot-linux/dist/pipewire-module.tar.gz https://github.com/djhardrich/schwung-chroot-linux/releases/download/v0.2.0/pipewire-module.tar.gz
curl -L -o schwung-chroot-linux/dist/pw-chroot-desktop.tar.gz https://github.com/djhardrich/schwung-chroot-linux/releases/download/v0.2.0/pw-chroot-desktop.tar.gz

cd schwung-chroot-linux && ./scripts/install.sh
cd ../schwung-norns && ./scripts/install.sh

ssh root@move.local 'sh /data/setup-norns.sh'
