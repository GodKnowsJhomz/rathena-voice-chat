# rAthena Voice Chat

**rAthena** with built-in **Proximity Voice Chat** — talk to nearby players in real-time inside Ragnarok Online.

[![Website](https://img.shields.io/badge/Website-sitecraft.in.th-blue?style=for-the-badge)](https://sitecraft.in.th/)
[![Discord](https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.com/invite/aTkZw9ZrQ9)

![Voice Chat in action](docs/preview.jpg)
![Voice Chat in action 2](docs/preview4.jpg)

---

## Features

- **Proximity Voice** — Automatically hear players near you on the map. The closer they are, the louder the voice.
- **Push to Talk (PTT)** — Hold a configurable key (default: `V`) to transmit.
- **Open Mic** — Toggle always-on microphone mode.
- **Party / Guild / Room channels** — Dedicated voice channels separate from proximity.
- **Direct Call** — Call any character by name privately via the Voice Call window.
- **Mic & Speaker volume sliders** — Adjustable directly in-game.
- **War Mode** — Separate voice behavior during Guild vs Guild / WoE events.
- **Low latency** — Built on WebSocket + Opus audio codec.

---

## How It Works

| Component | Description |
|-----------|-------------|
| **voice-server** | Standalone WebSocket server (`src/voice/`) that routes audio between players |
| **map-server hook** | `voice_bridge.cpp` sends player position & auth to voice-server via UDP |
| **Client DLL** | Injected into RO client — captures mic, encodes with Opus, plays back received audio |

Players within range on the same map hear each other automatically. Moving away fades the volume out.

### Proximity Range

| Distance | Volume |
|----------|--------|
| 0 – 1 cell | 100% (full volume) |
| 1 – 14 cells | Gradually fades out |
| > 14 cells | Silent (out of range) |

> Range is configurable via `proximity_full_range` and `proximity_max_range` in `conf/voice_athena.conf`.

---

## Requirements

| | Windows | Linux |
|---|---|---|
| **Build** | Visual Studio 2019+ | GCC / Clang, autotools |
| **Database** | MySQL / MariaDB | MySQL / MariaDB |
| **Voice deps** | bundled (libuv, uWebSockets) | `libuv1-dev` via apt |

---

## Installation

### Windows

1. Clone with submodules:
   ```bash
   git clone --recurse-submodules https://github.com/Sitecraft-Admin/rathena-voice-chat.git
   ```

2. Open `rAthena.sln` in Visual Studio and build all projects.

3. Configure `conf/voice_athena.conf`.

4. Start servers in order:
   ```
   login-server.exe
   char-server.exe
   map-server.exe
   voice-server.exe
   ```

5. Inject the client DLL into your Ragnarok Online client.

6. Log in — Voice Settings window will appear in-game.

### Linux

1. Clone and install dependencies (once):
   ```bash
   git clone https://github.com/Sitecraft-Admin/rathena-voice-chat.git
   cd rathena-voice-chat
   git submodule update --init 3rdparty/uWebSockets 3rdparty/uSockets
   ```

   **Ubuntu / Debian:**
   ```bash
   apt install libuv1-dev libmysqlclient-dev zlib1g-dev
   ```

   **AlmaLinux / Rocky / RHEL / CentOS:**
   ```bash
   sudo dnf config-manager --enable crb && sudo dnf install libuv-devel
   ```

2. Build:
   ```bash
   ./configure && make server
   ```

3. Configure `conf/voice_athena.conf`.

4. Start servers in order:
   ```bash
   ./login-server
   ./char-server
   ./map-server
   ./voice-server
   ```

5. Inject the client DLL into your Ragnarok Online client.

6. Log in — Voice Settings window will appear in-game.

---

## In-Game Voice Settings

| Setting | Description |
|---------|-------------|
| **Push to Talk** | Hold PTT key to speak |
| **Open Mic** | Always transmit voice |
| **PTT Key** | Click to rebind (default: `V`) |
| **Mic Volume** | Input gain slider |
| **Speaker Volume** | Output volume slider |
| **Players tab** | Per-player mute / volume control |
| **Devices tab** | Select audio input/output device |
| **Voice Call** | Direct private call to any character by name |

---

## Community

- 🌐 Website: [sitecraft.in.th](https://sitecraft.in.th/)
- 💬 Discord: [discord.com/invite/aTkZw9ZrQ9](https://discord.com/invite/aTkZw9ZrQ9)

---

## License

GPL-3.0 — Based on [rAthena](https://github.com/rathena/rathena) open-source project.
