# pi-audio-node

Turns a Raspberry Pi (+ audio HAT) into a modern AES67 audio device:

* **Receiver** — AES67/L24+L16, **ST 2022-7 seamless protection** over two
  network interfaces (sequence-merged jitter buffer), PTP-anchored playout to ALSA
* **Sender** — test tone / sweep / WAV / MP3, paced by the **PTP media clock**,
  identical-packet 2022-7 duplication on both interfaces, SDP with `a=group:DUP`
* **PTP analyzer** — own IEEE 1588-2008 slave with **full BMCA** (every announce
  source with its complete dataset and the field the decision fell on), offset
  history, path delay (E2E)
* **NMOS** — IS-04 registration (unicast DNS-SD via plain `res_query` + override),
  IS-05 with two transport-param legs, **IS-12 control protocol** with
  **BCP-008-01/-02** receiver/sender status monitors
* **Web UI** — touch-friendly dark SPA (dashboard, receiver, sender, PTP analyzer,
  settings), served by the built-in web server, shown fullscreen on the Pi's
  touchscreen via Chromium kiosk, and reachable from any browser

Single C++20 daemon, no heavyweight frameworks: civetweb, nlohmann-json,
minimp3, dr_wav (vendored) + ALSA and libsamplerate from apt.

## Install (Raspberry Pi OS, labwc desktop)

```
git clone https://github.com/Gemini2350/pi-audio-node.git
cd pi-audio-node && ./install.sh
```

That installs dependencies, builds, sets up the `pi-audio-node` systemd service
and the fullscreen kiosk UI, both starting at boot. Config lives in
`~/pi-audio-node.json`, audio files in `~/audio-files`.

## Notes

* PTP uses software timestamps (Pi 4 has no NIC timestamping): offset jitter is
  tens of microseconds - fine for AES67 media clocking, not a boundary clock.
* The IS-12 endpoint is `ws://<pi>:8080/x-nmos/ncp/v1.0`, advertised as a device
  control. Registration expects a registry announced via unicast DNS-SD
  (`_nmos-register._tcp` in the DNS search domain) or set `nmos.registry_override`.
* Ports 80/319/320 are bound via `cap_net_bind_service` (set by the installer).
