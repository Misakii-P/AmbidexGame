const express = require('express');
const http = require('http');
const path = require('path');
const fs = require('fs');
const net = require('net');
const { exec } = require('child_process');
const { Server } = require('socket.io');
const os = require('os');

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
  cors: {
    origin: '*',
    methods: ['GET', 'POST']
  }
});

function getLanIps() {
  const nets = os.networkInterfaces();
  const ips = [];
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) ips.push(net.address);
    }
  }
  return ips;
}

const indexHtml = fs.readFileSync(path.join(__dirname, 'public', 'index.html'), 'utf8');
app.get('/', (req, res) => {
  const ips = getLanIps();
  const label = '<strong>LAN IP' + (ips.length !== 1 ? 's' : '') + ':</strong> ' + (ips.length ? ips.join(', ') : '(none)');
  res.type('html').send(indexHtml.replace('{SERVER_IPS}', label));
});

app.use(express.static(path.join(__dirname, 'public')));

app.get('/api/server-ips', (req, res) => {
  res.json(getLanIps());
});

function computePairResult(vote1, vote2) {
  if (vote1 === 'ally' && vote2 === 'ally') return { c1: 2, c2: 2 };
  if (vote1 === 'ally' && vote2 === 'betray') return { c1: -2, c2: 3 };
  if (vote1 === 'betray' && vote2 === 'ally') return { c1: 3, c2: -2 };
  return { c1: 0, c2: 0 };
}

let hostSocketId = null;
let hostDisconnectTimer = null;
let players = [];
let slots = [];
let phase = 'lobby';
let currentRound = 0;
let pairings = [];
let allResults = [];

const SLOT_DEFS = [
  { id: 1, type: 'pair' },
  { id: 2, type: 'solo' },
  { id: 3, type: 'pair' },
  { id: 4, type: 'solo' },
  { id: 5, type: 'pair' },
  { id: 6, type: 'solo' },
];

function initSlots() {
  slots = SLOT_DEFS.map(s => ({
    id: s.id,
    type: s.type,
    name: '',
    playerIds: [],
    socketId: null,
    voted: false,
    vote: null,
    roundChange: null,
  }));
}

function resetGame() {
  players = [];
  initSlots();
  phase = 'lobby';
  currentRound = 0;
  pairings = [];
  allResults = [];
}

function getPlayerPoints(playerId) {
  const p = players.find(pl => pl.id === playerId);
  return p ? p.points : 0;
}

function setPlayerPoints(playerId, val) {
  const p = players.find(pl => pl.id === playerId);
  if (p) p.points = val;
}

function getState() {
  return {
    phase,
    players: players.map(p => ({ id: p.id, name: p.name, points: p.points, roundStartPoints: p.roundStartPoints })),
    slots: slots.map(s => ({
      id: s.id,
      type: s.type,
      name: s.name,
      playerIds: s.playerIds,
      connected: s.socketId !== null,
      voted: s.voted,
      vote: s.vote,
      roundChange: s.roundChange,
    })),
    currentRound,
    pairings: pairings.map(p => ({ slot1Id: p.slot1Id, slot2Id: p.slot2Id })),
    allResults,
    hostConnected: hostSocketId !== null,
  };
}

// --- Shared game logic functions used by both Socket.IO and TCP bridge ---

function sharedHostConnect(clientId, socket) {
  hostSocketId = clientId;
  if (hostDisconnectTimer) {
    clearTimeout(hostDisconnectTimer);
    hostDisconnectTimer = null;
  }
  if (socket && socket.emit) {
    socket.emit('server-ips', getLanIps());
  }
  broadcast();
}

function sharedSetupPlayers(clientId, playerNames) {
  if (clientId !== hostSocketId) return { error: 'Not host' };
  if (playerNames.length < 2 || playerNames.length > 9) return { error: 'Need 2-9 players' };
  players = playerNames.map((name, i) => ({
    id: i + 1,
    name: name.trim(),
    points: 3,
    roundStartPoints: 3,
  }));
  initSlots();
  phase = 'roundSetup';
  currentRound = 0;
  pairings = [];
  allResults = [];
  broadcast();
  return { success: true };
}

function sharedAssignSlots(clientId, assignment) {
  if (clientId !== hostSocketId) return { error: 'Not host' };
  if (phase !== 'roundSetup') return { error: 'Wrong phase' };
  initSlots();
  const usedPlayerIds = new Set();
  for (const a of assignment) {
    const slot = slots.find(s => s.id === a.slotId);
    if (!slot) continue;
    const ids = a.playerIds.filter(id => !usedPlayerIds.has(id) && players.find(p => p.id === id));
    if (ids.length === 0) continue;
    if (slot.type === 'pair' && ids.length !== 2) continue;
    if (slot.type === 'solo' && ids.length !== 1) continue;
    slot.playerIds = ids;
    ids.forEach(id => usedPlayerIds.add(id));
    const names = ids.map(id => { const p = players.find(pl => pl.id === id); return p ? p.name : '?' });
    slot.name = slot.type === 'pair' ? names.join(' & ') : names[0];
  }
  broadcast();
  return { success: true };
}

function sharedConfirmRoundSetup(clientId) {
  if (clientId !== hostSocketId) return { error: 'Not host' };
  if (phase !== 'roundSetup') return { error: 'Wrong phase' };
  const newPairings = [];
  for (let g = 0; g < 3; g++) {
    const pairSlot = slots[g * 2];
    const soloSlot = slots[g * 2 + 1];
    const pairFilled = pairSlot.playerIds.length > 0;
    const soloFilled = soloSlot.playerIds.length > 0;
    if (pairFilled !== soloFilled) {
      return { error: `Group ${g + 1}: pair and solo must both be filled or both empty` };
    }
    if (pairFilled && soloFilled) {
      newPairings.push({ slot1Id: pairSlot.id, slot2Id: soloSlot.id });
    }
  }
  if (newPairings.length === 0) return { error: 'Need at least one complete pair-solo group' };
  pairings = newPairings;
  phase = 'voting';
  currentRound++;
  players.forEach(p => { p.roundStartPoints = p.points; });
  slots.forEach(s => {
    s.voted = false;
    s.vote = null;
    s.roundChange = null;
  });
  allResults = [];
  broadcast();
  return { success: true };
}

function sharedJoinSlot(clientId, slotId) {
  const slot = slots.find(s => s.id === slotId);
  if (!slot) return { error: 'Slot not found' };
  if (slot.playerIds.length === 0) return { error: 'Slot is empty' };
  if (slot.socketId) return { error: 'Slot already has a device connected' };
  slot.socketId = clientId;
  broadcast();
  return { success: true };
}

function sharedVote(clientId, choice) {
  const slot = slots.find(s => s.socketId === clientId);
  if (!slot) return { error: 'You are not connected to a slot' };
  if (phase !== 'voting') return { error: 'Not voting phase' };
  if (slot.voted) return { error: 'Already voted' };
  if (choice !== 'ally' && choice !== 'betray') return { error: 'Invalid vote' };
  slot.voted = true;
  slot.vote = choice;

  const allVoted = pairings.every(p => {
    const s1 = slots.find(s => s.id === p.slot1Id);
    const s2 = slots.find(s => s.id === p.slot2Id);
    return s1 && s2 && s1.voted && s2.voted;
  });
  if (allVoted) {
    allResults = pairings.map(p => {
      const s1 = slots.find(s => s.id === p.slot1Id);
      const s2 = slots.find(s => s.id === p.slot2Id);
      const { c1, c2 } = computePairResult(s1.vote, s2.vote);
      s1.roundChange = c1;
      s2.roundChange = c2;
      const s1PlayerResults = s1.playerIds.map(pid => ({ playerId: pid, result: getPlayerPoints(pid) + c1 }));
      const s2PlayerResults = s2.playerIds.map(pid => ({ playerId: pid, result: getPlayerPoints(pid) + c2 }));
      return {
        slot1Id: p.slot1Id,
        slot2Id: p.slot2Id,
        slot1Vote: s1.vote,
        slot2Vote: s2.vote,
        slot1Change: c1,
        slot2Change: c2,
        slot1PlayerResults: s1PlayerResults,
        slot2PlayerResults: s2PlayerResults,
      };
    });
    phase = 'results';
  }
  broadcast();
  return { success: true };
}

function sharedFinishRound(clientId) {
  if (clientId !== hostSocketId) return { error: 'Not host' };
  if (phase !== 'results') return { error: 'Wrong phase' };
  allResults.forEach(r => {
    r.slot1PlayerResults.forEach(pr => setPlayerPoints(pr.playerId, pr.result));
    r.slot2PlayerResults.forEach(pr => setPlayerPoints(pr.playerId, pr.result));
  });
  phase = 'roundEnd';
  broadcast();
  return { success: true };
}

function sharedNextRound(clientId) {
  if (clientId !== hostSocketId) return { error: 'Not host' };
  phase = 'roundSetup';
  initSlots();
  pairings = [];
  allResults = [];
  broadcast();
  return { success: true };
}

function sharedBackToLobby(clientId) {
  if (clientId !== hostSocketId) return { error: 'Not host' };
  phase = 'lobby';
  broadcast();
  return { success: true };
}

function sharedResetGame(clientId) {
  if (clientId !== hostSocketId) return { error: 'Not host' };
  resetGame();
  broadcast();
  return { success: true };
}

function sharedDisconnect(clientId) {
  if (clientId === hostSocketId) {
    hostSocketId = null;
    hostDisconnectTimer = setTimeout(() => {
      server.close(() => process.exit(0));
    }, 10000);
  }
  const slot = slots.find(s => s.socketId === clientId);
  if (slot) {
    slot.socketId = null;
    slot.voted = false;
    slot.vote = null;
  }
  broadcast();
}

// --- TCP Bridge (for Wii U and other raw TCP clients) ---

const TCP_PORT = process.env.TCP_PORT || 3002;
let tcpClientId = 0;
const tcpClients = new Map(); // id -> { socket, buffer }

const tcpServer = net.createServer((sock) => {
  const id = ++tcpClientId;
  const clientId = 'tcp_' + id;
  tcpClients.set(id, { socket: sock, buffer: '' });

  sock.on('data', (data) => {
    const client = tcpClients.get(id);
    if (!client) return;
    client.buffer += data.toString();
    const lines = client.buffer.split('\n');
    client.buffer = lines.pop(); // Keep incomplete line in buffer
    for (const line of lines) {
      if (!line.trim()) continue;
      try {
        const msg = JSON.parse(line);
        handleTcpMessage(clientId, id, sock, msg);
      } catch (_) {
        // Invalid JSON, ignore
      }
    }
  });

  sock.on('close', () => {
    tcpClients.delete(id);
    sharedDisconnect(clientId);
  });

  sock.on('error', () => {
    tcpClients.delete(id);
    sharedDisconnect(clientId);
  });

  // Send initial state once connected
  const state = getState();
  const clientState = { ...state, isHost: false, mySlotId: null };
  sock.write(JSON.stringify(clientState) + '\n');
});

function handleTcpMessage(clientId, tcpId, sock, msg) {
  switch (msg.type) {
    case 'join-slot': {
      const r = sharedJoinSlot(clientId, msg.slotId);
      if (r.error) sock.write(JSON.stringify({ type: 'error', message: r.error }) + '\n');
      break;
    }
    case 'vote': {
      const r = sharedVote(clientId, msg.vote);
      if (r.error) {
        sock.write(JSON.stringify({ type: 'error', message: r.error }) + '\n');
      } else {
        sock.write(JSON.stringify({ type: 'vote-confirmed', vote: msg.vote }) + '\n');
      }
      break;
    }
    case 'ping': {
      sock.write(JSON.stringify({ type: 'pong' }) + '\n');
      break;
    }
  }
}

// --- Broadcast to all clients (Socket.IO + TCP Bridge) ---

function broadcast() {
  const state = getState();
  // Socket.IO clients
  for (const [id, socket] of io.sockets.sockets) {
    if (id === hostSocketId) {
      socket.emit('state-update', { ...state, isHost: true });
    } else {
      const slot = slots.find(s => s.socketId === id);
      socket.emit('state-update', { ...state, isHost: false, mySlotId: slot ? slot.id : null });
    }
  }
  // TCP clients
  for (const [id, client] of tcpClients) {
    const slot = slots.find(s => s.socketId === 'tcp_' + id);
    const clientState = { ...state, isHost: false, mySlotId: slot ? slot.id : null };
    try {
      client.socket.write(JSON.stringify(clientState) + '\n');
    } catch (_) {
      client.socket.destroy();
      tcpClients.delete(id);
    }
  }
}

io.on('connection', (socket) => {
  socket.on('host-connect', () => {
    sharedHostConnect(socket.id, socket);
  });

  socket.on('setup-players', (playerNames) => {
    const r = sharedSetupPlayers(socket.id, playerNames);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('assign-slots', (assignment) => {
    const r = sharedAssignSlots(socket.id, assignment);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('confirm-round-setup', () => {
    const r = sharedConfirmRoundSetup(socket.id);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('join-slot', (slotId) => {
    const r = sharedJoinSlot(socket.id, slotId);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('vote', (choice) => {
    const r = sharedVote(socket.id, choice);
    if (r && r.error) {
      socket.emit('error', r.error);
    } else {
      socket.emit('vote-confirmed', choice);
    }
  });

  socket.on('finish-round', () => {
    const r = sharedFinishRound(socket.id);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('next-round', () => {
    const r = sharedNextRound(socket.id);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('back-to-lobby', () => {
    const r = sharedBackToLobby(socket.id);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('reset-game', () => {
    const r = sharedResetGame(socket.id);
    if (r && r.error) socket.emit('error', r.error);
  });

  socket.on('disconnect', () => {
    sharedDisconnect(socket.id);
  });
});

function launchAppWindow() {
  const url = `http://127.0.0.1:${PORT}`;
  const la = process.env.LOCALAPPDATA || '';
  const pf = process.env.ProgramFiles || 'C:\\Program Files';
  const pfx86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
  const browsers = [
    la + '\\Google\\Chrome\\Application\\chrome.exe',
    pf + '\\Google\\Chrome\\Application\\chrome.exe',
    pfx86 + '\\Google\\Chrome\\Application\\chrome.exe',
    pfx86 + '\\Microsoft\\Edge\\Application\\msedge.exe',
    pf + '\\Microsoft\\Edge\\Application\\msedge.exe',
    la + '\\Microsoft\\Edge\\Application\\msedge.exe',
  ];
  for (const b of browsers) {
    try { fs.accessSync(b); exec(`"${b}" --app="${url}"`, { windowsHide: true }); return; } catch (_) {}
  }
  exec(`start "" "${url}"`);
}

server.on('error', (err) => {
  if (err.code === 'EADDRINUSE') {
    launchAppWindow();
    process.exit(0);
  } else {
    console.error('Server error:', err);
  }
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, '0.0.0.0', () => {
  console.log(`Ambidex game server running on http://0.0.0.0:${PORT}`);
  console.log(`TCP bridge (Wii U) on port ${TCP_PORT}`);
  launchAppWindow();
});

tcpServer.listen(TCP_PORT, '0.0.0.0', () => {
  console.log(`TCP bridge listening on 0.0.0.0:${TCP_PORT}`);
});
