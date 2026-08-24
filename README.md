# Ambidex Game

![Ambidex Game Logo](https://media.discordapp.net/attachments/1153393930872827945/1519425836774719658/ABgamelogo.png?ex=6a3d8315&is=6a3c3195&hm=d3d0da9278b862cea156379992a971a8ec0da030404832df85863b51521a2e3b&=&format=webp&quality=lossless&width=1216&height=684)
"So what's it going to be? Will you choose to ally? ...Or to betray?"
This web-based game tries to make a 1:1 recreation of the Ambidex game featured in *Zero Escape: Virtue's Last Reward*.

## What is the Ambidex Game?

The Ambidex Game is Kotaro Uchikoshi's vision of the Prisoner's Dilemma. You either work in a pair, or a solo, and there can only be up to three solo and pair groups. Each group of solos and pairs are put against the other in a game where they have to choose to Ally, or to Betray. If both teams ally, then each one of the members in the group get 2 points. If one chooses ally and gets betrayed, the betrayed participant(s) loses 2 points while the ones who betray get 3 points, if both betray, no points for any; and so on.

![Ambidex Game Table](https://i.ytimg.com/vi/bdcv-LF3eQg/hqdefault.jpg)

I thought it would be a highly interesting concept to take into a LAN game. Maybe you want to put your friend's trust at test, or perhaps you want to place 9 strangers together in one facility going against eachother, which seems like a nice sunday plan.

## Features

- **Desktop exe**: Runs as server + host UI in a frameless window (only allows one instance per computer, localhost exists for browser clients on port 3000)
- **Android APK**: Join-only client with a simple UI
- **Wii U Homebrew**: Join-only client with dual-screen support (TV + DRC)
- **Nintendo 3DS Homebrew**: Join-only client with dual-screen support (top + bottom)
- **LAN multiplayer**: No internet required, works over local network
- **Terminal-style UI**: Source Code Pro font, teal color scheme, retro terminal aesthetic
- **Ambidex gameplay**: Ally/Betray votes, slot pairings, real-time results reveal
- **OST Music Player**: Built-in player with VLR/999 soundtrack for the host.

## Quick Start

### Windows Exe

1. Download the latest `AmbidexGame.exe` from Releases
2. Run it — the first instance starts the server and opens the host UI
3. Players on the same network open `http://<host-ip>:3000` in a browser
4. Or use the Android APK to scan/connect

### Development

```bash
# Install dependencies
npm install

# Run server locally
npm start

# Build Windows exe
npm run build

# Build Android APK (requires Android SDK + Capacitor)
npx cap sync android
cd android
./gradlew assembleDebug

# Build Wii U homebrew (requires devkitPro with wiiu-dev)
cd wiiu
make

# Build 3DS homebrew (requires devkitPro with 3ds-dev)
cd 3ds
make
```

## Controls

- **HOST GAME**: Start a server and manage players, rounds and solo/pairs grouping.
- **JOIN TEAM**: Enter the host's LAN IP to connect as a player
- **Fullscreen toggle**: Bottom-right corner (PC only)
- **Music player**: Bottom-left corner in the host menu — select and play VLR/999 tracks

## Project Structure

```
ambidex-game/
├── server.js              # Express + Socket.IO server
├── public/                # Web client files (served statically)
│   ├── index.html         # Desktop/web client
│   ├── mobile.html        # Android client
│   ├── css/               # style.css + style-mobile.css
│   ├── js/game.js         # Game logic + socket handlers
│   ├── vendor/            # Vendored libs (socket.io.min.js)
│   ├── img/               # Backgrounds, logo, favicon
│   ├── icons/             # App/exe icons (used by build scripts)
│   ├── sfx/               # Sound effects (.wav)
│   └── music/             # OST tracks (.mp3)
├── android/               # Capacitor Android project
├── wiiu/                  # Wii U homebrew client (unfinished, gitignored)
│   ├── source/            # C source code
│   └── Makefile           # devkitPro build
├── 3ds/                   # Nintendo 3DS homebrew client
│   ├── source/            # C source code
│   ├── gfx/               # Screen images + tex3ds files
│   ├── romfs/             # Source WAVs (embedded at build time)
│   ├── tools/gen_wavs.ps1 # Regenerates source/wav_data.c
│   └── Makefile           # devkitARM build
├── dist/                  # Build outputs (gitignored)
├── scripts/
│   ├── prebuild.js        # Nexe icon patching
│   ├── patch-exe.js       # Icon + version resources via rcedit
│   ├── patch-gui.js       # Flips exe subsystem to GUI (no console)
│   └── make-ico.js / generate-icons.ps1  # Icon generation
└── capacitor.config.ts    # Capacitor configuration
```

## Tech Stack

- **Server**: Node.js, Express, Socket.IO
- **Client**: Vanilla JS, CSS, Socket.IO Client
- **Desktop**: Nexe (bundles server into Windows exe)
- **Android**: Capacitor v8 (wraps web client as native APK)
- **Wii U**: devkitPro + libctru + SDL2 (homebrew)
- **3DS**: devkitPro + libctru (homebrew, framebuffer rendering)

## Attribution

All Zero Escape series assets (music, images, sound effects) used in this
project are the property of **Spike Chunsoft** and **Chunsoft**. This is a
fan work and is not affiliated with or endorsed by them.

See [NOTICE.md](./NOTICE.md) for detailed attributions.

## License

MIT — © 2026 Misakii-P
