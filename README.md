# DP2PS — Distributed P2P Storage System

A lightweight peer-to-peer file storage and transfer system written in C. Nodes discover each other automatically over UDP broadcast, transfer files over TCP, and collaborate on large file downloads via a built-in chunk/swarm server — no central server required.

> Developed by **Rihan** (BSSE1630)

---

## Features

- **Zero-config peer discovery** — broadcast an `I_AM_NEW` message on startup; every peer on the LAN replies automatically
- **File transfer over TCP** — reliable, direct node-to-node file pushing
- **Swarm / chunked downloads** — large files are split into 512 KB chunks and fetched in parallel from every peer that holds the file
- **Persistent peer list** — known peers are saved to `peers.txt` and reloaded on the next run
- **Network sync** — a `sync` command finds the most up-to-date peer and pulls their peer list
- **Storage tracking** — each node declares how much disk space it wants to contribute; uploads are routed to the peer with the most available space
- **Cross-platform** — compiles and runs on Windows (Winsock2) and Linux/macOS (POSIX sockets + pthreads)

---

## How It Works

```
┌──────────┐   UDP broadcast (port 44679)   ┌──────────┐
│  Node A  │ ──── I_AM_NEW ───────────────► │  Node B  │
│          │ ◄─── UPDATE_LINE ──────────── │          │
│          │                                │          │
│          │   TCP (port 44678)             │          │
│          │ ──── file data ─────────────► │          │
│          │                                │          │
│          │   TCP chunks (port 44681)      │          │
│          │ ◄─── chunk 0 ──────────────── │  Node C  │
│          │ ◄─── chunk 1 ──────────────── │  Node D  │
└──────────┘                                └──────────┘
```

| Port  | Protocol | Purpose                        |
|-------|----------|--------------------------------|
| 44678 | TCP      | File transfers (push)          |
| 44679 | UDP      | Discovery & peer updates       |
| 44680 | UDP      | File request broadcasts        |
| 44681 | TCP      | Chunk server (swarm downloads) |

---

## Building

### Linux / macOS

```bash
gcc p2p.cpp -o p2p -lpthread
```

### Windows (MinGW)

```bash
gcc p2p.cpp -o p2p.exe -lws2_32
```

### Windows (MSVC)

```bash
cl p2p.cpp /link ws2_32.lib
```

---

## Usage

Run the program on each machine that should participate in the network:

```
./p2p
```

On first launch you will be asked how much storage (in MB) you want to contribute to the network. This is saved to `myself.txt` so you won't be asked again.

### Commands

| Command    | Description                                              |
|------------|----------------------------------------------------------|
| `sync`     | Find the most up-to-date peer and pull their peer list   |
| `list`     | Show all known peers, their IPs, storage, and files      |
| `upload`   | Send a file to the best available peer                   |
| `download` | Download a file from the swarm (parallel chunk fetch)    |
| `help`     | Show the command list                                    |
| `quit`     | Exit the program                                         |

### Example session

```
> sync
[Sync] Waiting 500ms...
[Winner] 192.168.1.5
[Sync] Sync completed successfully!

> list
  ========Known Peers (3)========
  [1] desktop-alice
   IP: 192.168.1.5
  ...

> upload
  File path: /home/user/video.mp4
    [Info] File size: 104857600 bytes (100.00 MB)
    [Selected] Peer: desktop-alice (192.168.1.5) with 512 MB available
    [Uploading] Sending to 192.168.1.5...
    [Success] Upload complete.

> download
  Enter file name : video.mp4
  [Swarm] File: 104857600 bytes | 200 chunks | 3 peers
  [Swarm] Downloading..........
  [Swarm] 'video.mp4' downloaded successfully!
```

---

## Peer Lookup

When specifying a peer for direct operations you can use any of:

- **Index number** — `1`, `2`, `3` (as shown by `list`)
- **IP address** — `192.168.1.5`
- **MAC address** — `aa:bb:cc:dd:ee:ff`
- **Hostname** — `desktop-alice`

---

## Persistent State

| File          | Contents                                        |
|---------------|-------------------------------------------------|
| `peers.txt`   | Known peers — loaded on startup, updated live   |
| `myself.txt`  | Your own node info and storage contribution     |
| `peers.tmp`   | Temporary file used during sync (auto-deleted)  |

---

## Limitations

- Designed for **LAN use** — UDP broadcast does not cross routers by default
- Maximum **50 peers** and **20 files per peer** (compile-time constants)
- The `I_AM_NEW` reply storm is noted as a known issue for large networks
- File server handles one incoming push at a time (chunk server is fully concurrent)

---

## License

This project was developed as an academic exercise. No license is currently specified — contact the author before reuse.
