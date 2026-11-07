# kia-can-bridge

ESP32 firmware that sniffs the Kia's CAN bus in **listen-only** mode and streams it over your phone hotspot, through a WireGuard tunnel, to server2.

```
Kia OBD-II ──CAN── transceiver ── ESP32 ──WiFi── phone hotspot ──LTE── home router ──UDP 51821── server2 (wg-kcan)
                                    │                                                              │
                              mark button                                              logger on 10.88.0.1:47000
```

Builds clean with PlatformIO espressif32 6.12.0 (ESP-IDF 5.5.0) for `esp32dev`.

## Safety properties

- TWAI is in `TWAI_MODE_LISTEN_ONLY` with the TX queue disabled. The controller never ACKs, never sends error frames, and there's no transmit call anywhere in the code.
- The tunnel only allows `10.88.0.1/32`. server2's firewall (below) only lets that interface reach the logger port.

## 1. Keys (on server2)

```bash
sudo apt install wireguard
cd /etc/wireguard && umask 077
wg genkey | tee server.key | wg pubkey > server.pub
wg genkey | tee esp32.key  | wg pubkey > esp32.pub
wg genpsk > kcan.psk
```

## 2. server2 tunnel: `/etc/wireguard/wg-kcan.conf`

```ini
[Interface]
Address    = 10.88.0.1/24
ListenPort = 51821
PrivateKey = <contents of server.key>
# Tunnel can reach the logger port and nothing else; never forwarded.
PostUp   = iptables -I INPUT -i %i -j DROP; iptables -I INPUT -i %i -p udp --dport 47000 -j ACCEPT; iptables -I FORWARD -i %i -j DROP
PostDown = iptables -D INPUT -i %i -p udp --dport 47000 -j ACCEPT; iptables -D INPUT -i %i -j DROP; iptables -D FORWARD -i %i -j DROP

[Peer]
# Kia ESP32
PublicKey    = <contents of esp32.pub>
PresharedKey = <contents of kcan.psk>
AllowedIPs   = 10.88.0.2/32
```

```bash
sudo systemctl enable --now wg-quick@wg-kcan
sudo wg show wg-kcan        # "latest handshake" appears once the ESP32 connects
```

This interface is separate from Tailscale and doesn't touch it.

If the ESP32 is ever lost: delete the `[Peer]` block, `sudo systemctl restart wg-quick@wg-kcan`, and generate a new key pair for the next one.

## 3. Router

- Forward **UDP 51821** to server2's LAN IP. Nothing else.
- Your home IP has to be reachable by name or stay fixed. If it changes, set up DDNS and put that hostname in the ESP32 config. The firmware re-resolves it when the tunnel has been down for 60 s.

## 4. Configure and build the firmware

This is a PlatformIO project (`platformio.ini`: `esp32dev`, `framework = espidf`).

```bash
pio run -t menuconfig          # → "Kia CAN bridge"
pio run -t upload -t monitor
```

In menuconfig set:

| Setting | Value |
|---|---|
| Hotspot SSID / password | your phone's hotspot |
| ESP32 private key | contents of `esp32.key` |
| server2 public key | contents of `server.pub` |
| Preshared key | contents of `kcan.psk` |
| Home endpoint / port | DDNS hostname or public IP, `51821` |
| TWAI TX/RX GPIO | your board's CAN pins |
| Bus bitrate | 500k for OBD-II pins 6/14 |
| Mark button GPIO | 0 = the BOOT button (default) |

The first build pulls `esphome/wireguard` from the ESP component registry.

`sdkconfig.esp32dev` now holds your hotspot password and private key, and `.gitignore` already excludes it. Don't commit it.

**PSRAM boards (e.g. S3 N8R8):** uncomment `CONFIG_SPIRAM=y` in `sdkconfig.defaults` and raise *Outage buffer size* to 65536. That gives about 1.5 MB, enough for minutes of dead zone instead of seconds.

## 5. Test in this order

1. **Network path on the bench, no car.** Turn on *Simulate CAN traffic*, flash, and plug into USB with the hotspot on. On server2:
   ```bash
   python3 tools/kcan_tool.py listen --bind 10.88.0.1 --csv /tmp/sim.csv
   ```
   You should see `device boot_id …`, packet counts climbing, `lost=0`, and `MARK #n` when you press BOOT. Turn the hotspot off for 30 s and back on: `ring_used` should climb, then drain, with no `records_dropped`.
2. **Server logger without hardware.** Claude Code can develop against the fake device: `python3 tools/kcan_tool.py fake --host 10.88.0.1 --loss 0.02` (or `127.0.0.1` while the logger binds there).
3. **Real bus.** Turn simulation off, set the bitrate, and plug into the OBD-II port. Check the STATUS line: `twai=1` (running), `frames_rx` climbing, and `twai_rx_missed` staying flat.

## Wiring notes

- OBD-II pin 6 = CAN-H, pin 14 = CAN-L, pin 4/5 = ground, pin 16 = +12 V always on.
- Use a 3.3 V transceiver (SN65HVD230 or similar, or the one on your Waveshare board). **Don't** add a 120 Ω terminator. The car's bus is already terminated.
- Pin 16 is live with the key off. The ESP32 on WiFi draws roughly 1 Ah/day from the battery. Unplug it when you're done, or switch it off ignition power.
- On 2015-era Kia, OBD-II pins 6/14 are usually powertrain CAN. Body functions (locks, lights, switches) may live on a separate, slower bus that isn't on the port. If your button presses show nothing, that's the likely reason.

## Files

```
src/proto.h          wire format (shared contract)
src/can_capture.c    TWAI listen-only, change-only filter, mark button, sim mode
src/ring.c           lock-free outage buffer
src/net.c            WiFi → SNTP → WireGuard supervisor
src/sender.c         batches buffer → UDP, status packets
src/Kconfig.projbuild  all settings
PROTOCOL.md          contract for the server2 logger (give this to Claude Code)
tools/kcan_tool.py   reference decoder + fake device
```
