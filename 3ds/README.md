# Ambidex Game - Nintendo 3DS (Homebrew)

Nintendo 3DS client for the Ambidex Game server. Connects via TCP to a PC-hosted
game server, allowing 3DS players to join LAN games alongside desktop/mobile/Wii U players.

## Requirements

- [devkitPro](https://devkitpro.org/) with `3ds-dev` package installed
- A Nintendo 3DS running custom firmware (Luma3DS recommended)

### Install devkitPro

```
# Windows: download and run the graphical installer from devkitpro.org
# Then open MSYS2 terminal and run:
pacman -Syu 3ds-dev --needed
```

## Build

```
cd 3ds
make
```

Output: `ambidex.3dsx` — copy to `sd:/3ds/` on your 3DS SD card.

## Usage

1. Start the server on your PC: `node server.js` (from the project root).
2. On the 3DS, launch Ambidex Game from the Homebrew Launcher.
3. Tap the IP field (or press A) to open the on-screen keyboard, enter the host IP, then select OK.
4. Tap a free slot card to join, then tap START when the polling warning appears and vote.

## Controls

- **Touch**: Edit IP / Connect / Join slot card / START vote / Confirm vote
- **A**: Connect / Join selected slot / Confirm vote
- **D-pad**: Navigate slot grid and vote options
- **B**: Back / Leave game / Cancel
- **Start**: Exit application

## Project Structure

```
3ds/
├── gfx/              # Background PNGs + tex3ds .t3s files
│   ├── topbg.png     # Top screen background (400x240)
│   ├── topbg.t3s
│   ├── botbg.png     # Bottom screen background (320x240)
│   └── botbg.t3s
├── romfs/            # Sound effects source WAVs (PCM16)
├── tools/
│   └── gen_wavs.ps1  # Regenerates source/wav_data.c from romfs/
├── source/
│   ├── main.c        # Single-file client
│   └── wav_data.c    # GENERATED: wav bytes embedded as C arrays
├── Makefile          # devkitARM build (tex3ds + citro2d)
└── README.md
```

## How it works

The server listens on port **3002** for raw TCP connections (in addition to the
existing Socket.IO on 3000). The 3DS connects via TCP and sends/receives newline-
delimited JSON messages:

- `{"type":"join-slot","slotId":N}` — attach this device to a player-assigned slot
- `{"type":"vote","vote":"ally"|"betray"}` — submit vote during the voting phase
- Server broadcasts full state as JSON on every change, including a per-client `mySlotId`

The client mirrors the current web/mobile game flow: lobby → slot select → round
setup → voting (Ally/Betray) → results reveal → standings. Sound effects match the
other clients (title, connected, selection, start, vote, resound, wrong).

## Tech Stack

- **Graphics**: citro2d / citro3d (tex3ds sprite sheets)
- **Audio**: ndsp (embedded PCM16, see below)
- **Networking**: socInit (BSD TCP sockets)
- **Build**: devkitARM + tex3ds

## Audio

Sounds are compiled into the binary — no romfs or SD files are needed, which
keeps playback working in every launch environment (Citra/Azahar, any HBL
version). To change a sound effect:

1. Replace the matching `.wav` in `romfs/` (must be PCM16, mono/stereo)
2. Regenerate the embedded data: `powershell -File tools\gen_wavs.ps1`
3. `make`

Playback design: all clips are decoded once at boot into DSP-accessible linear
memory (~620 kB) and kept resident. Channel 0 = title music, channel 1 = one-shot
SFX, so sounds overlap like on the web client. Triggering a sound only queues an
ndsp wave buffer — no file I/O, allocation, or per-frame audio work.

## Notes

- The 3DS client is **player-only** (no host features).
- Built following the same patterns as Booru3DS and 3ds-game projects.
