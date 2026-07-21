#!/bin/bash
# ------------------------------------------------------------------------------
# pi-audio-node installer for Raspberry Pi OS (Bookworm/Trixie, labwc desktop)
#
#   ./install.sh          install deps, build, install, enable service + kiosk
#   ./install.sh --build  build + install only (no autostart changes)
#
# Fresh Pi:
#   git clone https://github.com/Gemini2350/pi-audio-node.git
#   cd pi-audio-node && ./install.sh
# ------------------------------------------------------------------------------
set -e
cd "$(dirname "$0")"

echo "== dependencies =="
sudo apt-get update -qq
sudo apt-get install -y -q build-essential cmake git libasound2-dev libsamplerate0-dev chromium >/dev/null

echo "== build =="
cmake -B build -DCMAKE_BUILD_TYPE=Release . >/dev/null
cmake --build build -j"$(nproc)"

echo "== install =="
sudo cmake --install build >/dev/null
sudo setcap 'cap_net_bind_service,cap_sys_nice+ep' /usr/local/bin/pi-audio-node
mkdir -p ~/audio-files

if [ "$1" = "--build" ]; then
    echo "build + install done."
    exit 0
fi

echo "== systemd service =="
sudo tee /etc/systemd/system/pi-audio-node.service >/dev/null <<EOF
[Unit]
Description=pi-audio-node AES67 engine
After=network-online.target sound.target
Wants=network-online.target

[Service]
User=$USER
ExecStart=/usr/local/bin/pi-audio-node /home/$USER/pi-audio-node.json
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now pi-audio-node

echo "== kiosk autostart =="
mkdir -p ~/.config/labwc
touch ~/.config/labwc/autostart
sed -i '/pamstart/d;/pi-audio-kiosk/d' ~/.config/labwc/autostart
cat > ~/pi-audio-kiosk.sh <<'EOF'
#!/bin/bash
# wait for the engine, then run the ui fullscreen on the touchscreen
until curl -sf http://localhost/api/status >/dev/null; do sleep 1; done
exec chromium --kiosk --noerrdialogs --disable-infobars --disable-session-crashed-bubble \
    --check-for-update-interval=31536000 --password-store=basic --disable-features=Translate \
    --ozone-platform=wayland http://localhost/
EOF
chmod +x ~/pi-audio-kiosk.sh
echo "/home/$USER/pi-audio-kiosk.sh &" >> ~/.config/labwc/autostart

echo ""
echo "Done. Engine runs as systemd service 'pi-audio-node',"
echo "kiosk ui starts with the desktop. Reboot to see it all come up."
