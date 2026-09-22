#!/usr/bin/env bash
# Sets up the Project Predation lobby server on a Linux machine, and keeps it running.
#
# Made for a fresh Ubuntu machine on Oracle Cloud's free tier (see Docs/SERVER.md), and works on any
# Ubuntu or Debian machine with a public address. Run it on the server:
#
#   curl -fsSL https://raw.githubusercontent.com/jojozagjos/Project-Predation/main/Tools/LobbyServer/setup-linux.sh | bash
#
# Running it again updates the server to the newest code and restarts it. Nothing else to do.
#
# What it does:
#   1. installs a C++ compiler and git
#   2. downloads only the five source files the server needs (not the whole game)
#   3. builds the server
#   4. installs it as a service, so it starts when the machine starts and restarts if it ever stops
#   5. opens UDP port 27020 in the machine's own firewall
set -euo pipefail

PORT="${PORT:-27020}"
REPO="${REPO:-https://github.com/jojozagjos/Project-Predation.git}"
BRANCH="${BRANCH:-main}"
DIR=/opt/predation-lobby
SERVICE=predation-lobby

say() { printf '\n== %s\n' "$*"; }

say "Installing a compiler and git"
sudo apt-get update -y -qq
sudo apt-get install -y -qq g++ git >/dev/null

say "Getting the source"
sudo mkdir -p "$DIR"
sudo chown "$(id -u):$(id -g)" "$DIR"
if [ -d "$DIR/src/.git" ]; then
    git -C "$DIR/src" fetch --depth 1 origin "$BRANCH"
    git -C "$DIR/src" reset --hard "origin/$BRANCH"
else
    # Only the folders the server is built from: the game's models and sounds are hundreds of
    # megabytes the server has no use for.
    git clone --depth 1 --branch "$BRANCH" --filter=blob:none --sparse "$REPO" "$DIR/src"
    git -C "$DIR/src" sparse-checkout set Engine/Net Tools/LobbyServer
fi

say "Building"
cd "$DIR/src"
g++ -std=c++20 -O2 -I. \
    Tools/LobbyServer/main.cpp \
    Engine/Net/LobbyServer.cpp Engine/Net/LobbyDirectory.cpp Engine/Net/LobbyProtocol.cpp \
    Engine/Net/SocketSystem.cpp \
    -o "$DIR/predation-lobby-server.new"
mv "$DIR/predation-lobby-server.new" "$DIR/predation-lobby-server"

say "Installing the service"
sudo tee "/etc/systemd/system/$SERVICE.service" >/dev/null <<EOF
[Unit]
Description=Project Predation lobby server
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=$DIR/predation-lobby-server --port $PORT
Restart=always
RestartSec=2
# Runs as a throwaway user with no home and no rights: it only needs its one port.
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE" >/dev/null 2>&1
sudo systemctl restart "$SERVICE"

say "Opening UDP port $PORT on this machine"
# Oracle's Ubuntu images block everything but SSH with iptables rules of their own, on top of the
# cloud's network rules (which have to be opened in Oracle's web console too; see Docs/SERVER.md).
if command -v iptables >/dev/null 2>&1; then
    if ! sudo iptables -C INPUT -p udp --dport "$PORT" -j ACCEPT 2>/dev/null; then
        sudo iptables -I INPUT 1 -p udp --dport "$PORT" -j ACCEPT
    fi
    if command -v netfilter-persistent >/dev/null 2>&1; then
        sudo netfilter-persistent save >/dev/null 2>&1 || true
    fi
fi
if command -v ufw >/dev/null 2>&1 && sudo ufw status 2>/dev/null | grep -q "Status: active"; then
    sudo ufw allow "$PORT/udp" >/dev/null
fi

sleep 1
say "Done"
sudo systemctl --no-pager --lines=5 status "$SERVICE" || true
ADDRESS="$(curl -fsS -4 https://ifconfig.me 2>/dev/null || true)"
echo
echo "The lobby server is running on UDP port $PORT."
if [ -n "$ADDRESS" ]; then
    echo "Its address is $ADDRESS -- put that in the game under Settings > Multiplayer > Lobby server."
fi
echo "To watch what it is doing:  sudo journalctl -u $SERVICE -f"
