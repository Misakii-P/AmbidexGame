let socket = null;
let state = null;
let hostMode = false;
let revealTimer = null;
let revealState = 'idle';
let revealPos = -1;
let voteReady = false;

function $(id) { return document.getElementById(id); }
function show(id) {
  document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
  const el = typeof id === 'string' ? $(id) : id;
  if (el) el.classList.add('active');
}

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

function playVoteSound() {
  if (!hostMode) return;
  const a = $('voteSound');
  if (a) { a.currentTime = 0; a.play().catch(() => {}); }
}

function playResultSound() {
  if (!hostMode) return;
  const a = $('resultSound');
  if (a) { a.currentTime = 0; a.play().catch(() => {}); }
}

function playSound(id) {
  const a = $(id);
  if (a) { a.currentTime = 0; a.play().catch(() => {}); }
}

function playTitle() {
  const a = $('titleSound');
  if (a) { a.loop = false; a.currentTime = 0; a.play().catch(() => {}); }
}

function stopTitle() {
  const a = $('titleSound');
  if (a) { a.pause(); a.currentTime = 0; }
}

function playSelectionSound() {
  playSound('selectionSound');
}

// ---------- Socket Setup ----------
function setupSocket(sock) {
  sock.on('state-update', (s) => {
    const prevPhase = state ? state.phase : null;
    state = s;
    render();
    if (hostMode && prevPhase !== 'results' && s.phase === 'results') {
      revealState = 'idle';
      revealPos = -1;
      startReveal();
    }
    if (prevPhase !== 'voting' && s.phase === 'voting') {
      voteReady = false;
    }
    if (prevPhase !== 'roundEnd' && s.phase === 'roundEnd') {
      stopReveal();
    }
  });

  sock.on('vote-confirmed', () => {});

  sock.on('connect', () => {
    stopTitle();
    if (!hostMode) playSound('connectedSound');
    if (hostMode) sock.emit('host-connect');
  });

  sock.on('error', (msg) => { alert(msg); });

  sock.on('disconnect', () => {});

  sock.on('server-ips', (ips) => {
    const el = $('serverIps');
    if (el) el.innerHTML = '<strong>LAN IP' + (ips.length > 1 ? 's' : '') + ':</strong> ' + ips.join(', ');
  });
}

function fetchServerIps() {
  const el = $('serverIps');
  if (!el) return;
  el.innerHTML = 'Detecting LAN IP...';
  fetch('/api/server-ips').then(r => r.json()).then(ips => {
    if (ips.length) el.innerHTML = '<strong>LAN IP' + (ips.length > 1 ? 's' : '') + ':</strong> ' + ips.join(', ');
    else el.innerHTML = '<strong>LAN IPs:</strong> (none detected)';
  }).catch(err => {
    el.innerHTML = '<strong>LAN IPs:</strong> (error: ' + err.message + ')';
  });
}

// ---------- Connection ----------
function connectAsHost() {
  stopTitle();
  hostMode = true;
  socket = io(window.location.origin);
  setupSocket(socket);
  fetchServerIps();
  show('hostSetup');
  renderHostSetup();
  initMusicPlayer();
}

function connectAsPlayer() {
  stopTitle();
  playSelectionSound();
  const ip = $('serverIpInput').value.trim();
  if (!ip) return alert('Enter server IP');
  const url = ip.includes('://') ? ip : 'http://' + ip + ':3000';
  socket = io(url, { transports: ['websocket'] });
  setupSocket(socket);
  socket.on('connect_error', (err) => { playSound('wrongSound'); alert('Connection error: ' + err.message); });
  show('playerJoin');
}

function showJoin() {
  playSelectionSound();
  $('joinSection').style.display = 'block';
}

function emit(ev) {
  playSelectionSound();
  socket.emit(ev);
}

// ---------- Auto Reveal (host only) ----------
function startReveal() {
  stopReveal();
  revealState = 'votes';
  revealPos = -1;
  renderResultsTable();
  scheduleVote();
}

function stopReveal() {
  if (revealTimer) { clearTimeout(revealTimer); revealTimer = null; }
}

function scheduleVote() {
  revealTimer = setTimeout(() => {
    const revealedOrder = state.slots.filter(s => s.playerIds.length > 0);
    revealPos++;
    if (revealPos >= revealedOrder.length) {
      revealState = 'done';
      playResultSound();
      renderResultsTable();
    } else {
      playVoteSound();
      renderResultsTable();
      scheduleVote();
    }
  }, 1000);
}

// ---------- Render ----------
function render() {
  if (!state) return;
  if (hostMode) renderHost();
  else renderPlayer();
}

// ---------- Host ----------
let hostLastPhase = null;

function renderHost() {
  const entering = hostLastPhase !== state.phase;
  hostLastPhase = state.phase;

  document.body.classList.toggle('host-results', state.phase === 'results');

  if (state.players.length === 0) {
    show('hostSetup');
    renderHostSetup();
    return;
  }

  switch (state.phase) {
    case 'lobby':
      show('hostLobby'); renderHostLobby(); break;
    case 'roundSetup':
      show('hostRoundSetup');
      $('roundSetupNum').textContent = state.currentRound + 1;
      if (entering) slotDraft = {};
      renderSlotAssignment(); break;
    case 'voting':
      show('hostVoting');
      $('voteRoundNum').textContent = state.currentRound;
      renderVotingProgress(); break;
    case 'results':
      show('hostResults');
      $('resultsRoundNum').textContent = state.currentRound;
      renderResultsTable(); break;
    case 'roundEnd':
      show('hostRoundEnd');
      $('endRoundNum').textContent = state.currentRound;
      renderRoundEnd(); break;
    default:
      show('hostLobby'); renderHostLobby();
  }
}

function renderHostLobby() {
  $('hostLobbyList').innerHTML = state.players.map(p => `
    <div class="team-card">
      <div>
        <span class="team-name">${esc(p.name)}</span>
      </div>
      <div>
        <span style="margin-left:12px;color:#00d4ff;font-weight:700">${p.points} pt</span>
      </div>
    </div>
  `).join('');
}

// ---------- Slot Assignment (Round Setup) ----------
// Local state so selection survives state-update re-renders
let slotDraft = {}; // { slotId: [playerId, ...] }

function renderSlotAssignment() {
  const grid = $('slotAssignmentGrid');
  grid.innerHTML = '';

  if (!state.slots) return;
  if (!state.players) return;

  // Build a set of player IDs already assigned in the draft
  const draftUsed = new Set();
  Object.values(slotDraft).forEach(ids => ids.forEach(id => { if (id) draftUsed.add(id); }));

  for (let g = 0; g < 3; g++) {
    const pairSlot = state.slots[g * 2];
    const soloSlot = state.slots[g * 2 + 1];
    if (!pairSlot || !soloSlot) continue;

    const div = document.createElement('div');
    div.className = 'slot-group';

    div.innerHTML += `<div class="slot-card">
      <div class="slot-type tag-pair">PAIR</div>
      <div style="margin-bottom:6px">
        <span class="connected-dot ${pairSlot.connected ? 'online' : 'offline'}"></span>
        <span class="slot-name">${esc(pairSlot.name || '')}</span>
      </div>
      ${buildSlotSelects(pairSlot.id, 'pair', draftUsed)}
    </div>`;

    div.innerHTML += `<div class="slot-card">
      <div class="slot-type tag-solo">SOLO</div>
      <div style="margin-bottom:6px">
        <span class="connected-dot ${soloSlot.connected ? 'online' : 'offline'}"></span>
        <span class="slot-name">${esc(soloSlot.name || '')}</span>
      </div>
      ${buildSlotSelects(soloSlot.id, 'solo', draftUsed)}
    </div>`;

    grid.appendChild(div);
  }

  // Bind change events to update draft
  grid.querySelectorAll('.slot-select').forEach(sel => {
    sel.addEventListener('change', (e) => {
      const slotId = parseInt(e.target.dataset.slot);
      const idx = parseInt(e.target.dataset.idx);
      const val = e.target.value ? parseInt(e.target.value) : null;
      if (!slotDraft[slotId]) slotDraft[slotId] = [];
      slotDraft[slotId][idx] = val;
    });
  });
}

function buildSlotSelects(slotId, type, draftUsed) {
  const count = type === 'pair' ? 2 : 1;
  const draft = slotDraft[slotId] || [];
  let html = '';
  for (let i = 0; i < count; i++) {
    const val = draft[i] || '';
    // Players used by OTHER slots in the draft
    const otherUsed = new Set();
    for (const [sid, ids] of Object.entries(slotDraft)) {
      if (parseInt(sid) !== slotId) {
        ids.forEach(id => { if (id) otherUsed.add(id); });
      }
    }
    html += `<select data-slot="${slotId}" data-idx="${i}" class="slot-select">
      <option value="">—</option>`;
    state.players.forEach(p => {
      if (!otherUsed.has(p.id) || val === p.id) {
        html += `<option value="${p.id}" ${val === p.id ? 'selected' : ''}>${esc(p.name)} (${p.points}pt)</option>`;
      }
    });
    html += `</select>`;
  }
  return html;
}

function confirmRoundSetup() {
  playSelectionSound();
  const result = [];
  for (const [slotIdStr, ids] of Object.entries(slotDraft)) {
    const slotId = parseInt(slotIdStr);
    const clean = ids.filter(id => id !== null && id !== undefined);
    if (clean.length === 0) continue;
    const slot = state.slots.find(s => s.id === slotId);
    if (!slot) continue;
    if (slot.type === 'pair' && clean.length !== 2) {
      alert('PAIR slots need exactly 2 players');
      return;
    }
    if (slot.type === 'solo' && clean.length !== 1) {
      alert('SOLO slots need exactly 1 player');
      return;
    }
    result.push({ slotId, playerIds: clean });
  }

  const filledCount = result.length;
  if (filledCount < 2) {
    alert('Need at least 2 filled slots');
    return;
  }

  const usedPlayerIds = new Set();
  for (const r of result) {
    for (const id of r.playerIds) {
      if (usedPlayerIds.has(id)) {
        alert('A player cannot be in multiple slots');
        return;
      }
      usedPlayerIds.add(id);
    }
  }

  socket.emit('assign-slots', result);
  socket.emit('confirm-round-setup');
}

// ---------- Host Voting ----------
function renderVotingProgress() {
  const activeSlots = state.slots.filter(s => s.playerIds.length > 0);
  let voted = 0;
  $('votingProgress').innerHTML = activeSlots.map(s => {
    const p = state.pairings.find(p => p.slot1Id === s.id || p.slot2Id === s.id);
    if (!p) return '';
    const opp = state.slots.find(o => o.id === (p.slot1Id === s.id ? p.slot2Id : p.slot1Id));
    if (s.voted) voted++;
    return `
      <div class="team-card">
        <div>
          <span class="connected-dot ${s.connected ? 'online' : 'offline'}"></span>
          <span class="team-name">${esc(s.name)}</span>
          <span class="team-type ${s.type}" style="margin-left:8px">${s.type}</span>
        </div>
        <div>
          <span style="color:#576574">vs ${esc(opp ? opp.name : '?')}</span>
          ${s.voted ? '<span class="vote-display ally" style="margin-left:10px">VOTED</span>' : '<span style="color:#576574;margin-left:10px">waiting...</span>'}
        </div>
      </div>
    `;
  }).join('');
  $('voteProgressText').textContent = `${voted} / ${state.pairings.length * 2} voted`;
}

// ---------- Host Results ----------
function getSlotResult(slotId) {
  return state.allResults ? state.allResults.find(r => r.slot1Id === slotId || r.slot2Id === slotId) : null;
}

function renderResultsTable() {
  const el = $('resultsContainer');
  const fb = $('finishRoundBtn');
  const slots = state.slots;
  const voting = revealState === 'votes';
  const done = revealState === 'done';
  const revealedOrder = slots.filter(s => s.playerIds.length > 0);

  // Template coordinates (from guidance.txt, within the image's pixel grid)
  const COLUMNS = [
    { x: 78, w: 125 },  // Pair 1
    { x: 209, w: 59 },  // Solo 1
    { x: 278, w: 126 }, // Pair 2
    { x: 409, w: 60 },  // Solo 2
    { x: 479, w: 125 }, // Pair 3
    { x: 609, w: 59 },  // Solo 3
  ];

  const ROWS = {
    team:  { y: 50, h: 36 },
    bp:    { y: 92, h: 36 },
    sel:   { y: 134, h: 36 },
    chg:   { y: 176, h: 37 },
    res:   { y: 219, h: 33 },
  };

  let parts = [];

  function pos(x, y, w, h, content, cls) {
    parts.push({ x, y, w, h, content, cls });
  }

  // Row: Team names (y=50-86)
  slots.forEach((s, i) => {
    if (s.playerIds.length === 0) return;
    const c = COLUMNS[i];
    pos(c.x, ROWS.team.y, c.w, ROWS.team.h, esc(s.name), 'over-team');
  });

  // Row: BP — points per player (y=92-128)
  slots.forEach((s, i) => {
    if (s.playerIds.length === 0) return;
    const c = COLUMNS[i];
    if (s.type === 'pair') {
      const bw = c.w / 2;
      s.playerIds.forEach((pid, j) => {
        const p = state.players.find(pl => pl.id === pid);
        pos(c.x + j * bw, ROWS.bp.y, bw, ROWS.bp.h,
          p ? p.roundStartPoints : '',
          'over-bp');
      });
    } else {
      const pid = s.playerIds[0];
      const p = state.players.find(pl => pl.id === pid);
      pos(c.x, ROWS.bp.y, c.w, ROWS.bp.h,
        p ? p.roundStartPoints : '',
        'over-bp');
    }
  });

  // Row: Select (y=134-170)
  slots.forEach((s, i) => {
    if (s.playerIds.length === 0) return;
    const c = COLUMNS[i];
    const r = getSlotResult(s.id);
    const idx = revealedOrder.indexOf(s);
    const show = done || (voting && idx >= 0 && idx <= revealPos);
    let content = '<span class="reveal-dot">?</span>';
    let cls = 'over-select';
    if (r && show) {
      const isFresh = voting && idx === revealPos;
      const slotVote = r.slot1Id === s.id ? r.slot1Vote : r.slot2Vote;
      content = `<span class="vote-display ${slotVote}">${slotVote}</span>`;
      if (isFresh) cls += ' new-reveal';
    }
    pos(c.x, ROWS.sel.y, c.w, ROWS.sel.h, content, cls);
  });

  // Row: Change (y=176-213)
  slots.forEach((s, i) => {
    if (s.playerIds.length === 0) return;
    const c = COLUMNS[i];
    let content = '';
    let cls = 'over-change';
    if (done) {
      const r = getSlotResult(s.id);
      if (r) {
        const ch = r.slot1Id === s.id ? r.slot1Change : r.slot2Change;
        const sign = ch > 0 ? '+' : '';
        content = `<span class="chg ${ch > 0 ? 'positive' : ch < 0 ? 'negative' : ''}">${sign}${ch}</span>`;
        cls += ' new-reveal';
      }
    }
    pos(c.x, ROWS.chg.y, c.w, ROWS.chg.h, content, cls);
  });

  // Row: Results (y=219-252)
  slots.forEach((s, i) => {
    if (s.playerIds.length === 0) return;
    const c = COLUMNS[i];
    if (s.type === 'pair') {
      const bw = c.w / 2;
      s.playerIds.forEach((pid, j) => {
        let content = '';
        let cls = 'over-res';
        if (done) {
          const r = getSlotResult(s.id);
          if (r) {
            const results = r.slot1Id === s.id ? r.slot1PlayerResults : r.slot2PlayerResults;
            const pr = results.find(x => x.playerId === pid);
            content = pr ? pr.result : '';
            cls += ' new-reveal';
          }
        }
        pos(c.x + j * bw, ROWS.res.y, bw, ROWS.res.h, content, cls);
      });
    } else {
      let content = '';
      let cls = 'over-res';
      if (done) {
        const r = getSlotResult(s.id);
        if (r) {
          const results = r.slot1Id === s.id ? r.slot1PlayerResults : r.slot2PlayerResults;
          const pr = results[0];
          content = pr ? pr.result : '';
          cls += ' new-reveal';
        }
      }
      pos(c.x, ROWS.res.y, c.w, ROWS.res.h, content, cls);
    }
  });

  // Build: image as template background + overlaid values
  let html = '<div class="results-wrap">';
  html += '<img src="/img/results-template.jpg" class="results-bg" alt="">';
  html += '<div class="results-overlay">';
  parts.forEach(p => {
    html += `<div class="over-cell ${p.cls}" style="left:${p.x}px;top:${p.y}px;width:${p.w}px;height:${p.h}px">${p.content}</div>`;
  });
  html += '</div></div>';
  el.innerHTML = html;
  fb.style.display = done ? 'inline-block' : 'none';
}

function finishRound() {
  playSelectionSound();
  stopReveal();
  socket.emit('finish-round');
}

function renderRoundEnd() {
  const sorted = [...state.players].sort((a, b) => b.points - a.points);
  $('roundEndPoints').innerHTML = sorted.map(p => `
    <div class="team-card">
      <div>
        <span class="team-name">${esc(p.name)}</span>
      </div>
      <div class="team-points">${p.points}</div>
    </div>
  `).join('');
}

// ---------- Host Setup ----------
let setupPlayers = ['Player 1', 'Player 2'];

function renderHostSetup() {
  $('hostSetupList').innerHTML = setupPlayers.map((name, i) => `
    <div class="team-input-row">
      <input type="text" value="${esc(name)}" data-idx="${i}" class="setup-name" placeholder="Player name">
    </div>
  `).join('');
  document.querySelectorAll('.setup-name').forEach(inp => {
    inp.addEventListener('input', (e) => {
      setupPlayers[parseInt(e.target.dataset.idx)] = e.target.value;
    });
  });
}

function addPlayer() {
  playSelectionSound();
  if (setupPlayers.length >= 9) return;
  setupPlayers.push('Player ' + (setupPlayers.length + 1));
  renderHostSetup();
}

function removePlayer() {
  playSelectionSound();
  if (setupPlayers.length <= 2) return;
  setupPlayers.pop();
  renderHostSetup();
}

function submitPlayers() {
  playSelectionSound();
  const names = setupPlayers.map(n => n.trim()).filter(n => n);
  if (names.length < 2) return alert('Need at least 2 players');
  socket.emit('setup-players', names);
}

// ---------- Player ----------
function renderPlayer() {
  if (!state || state.players.length === 0) {
    show('playerJoin');
    return;
  }
  const myId = state.mySlotId;
  if (!myId) {
    show('playerJoin');
    renderSlotSelect();
    return;
  }
  const slot = state.slots.find(s => s.id === myId);
  if (!slot) return;

  const slotPlayers = slot.playerIds.map(pid => state.players.find(p => p.id === pid)).filter(p => p);
  const bannerHtml = `${esc(slot.name)} <span class="type-badge team-type ${slot.type}" style="font-size:0.8rem;letter-spacing:2px;text-transform:uppercase;padding:2px 10px;border:1px solid;margin-left:10px">${slot.type}</span>`;

  const pointsHtml = slotPlayers.map(p => `<div class="player-point-item"><div class="player-point-name">${esc(p.name)}</div><div class="player-point-val">${p.points}</div></div>`).join('');

  switch (state.phase) {
    case 'lobby':
    case 'roundSetup':
    case 'pairing':
      show('playerWaiting');
      $('playerSlotBanner').innerHTML = bannerHtml;
      $('playerWaitPoints').innerHTML = pointsHtml;
      $('playerWaitMsg').textContent = 'Waiting for round to start...';
      break;
    case 'voting':
      if (slot.voted) {
        show('playerWaiting');
        $('playerSlotBanner').innerHTML = bannerHtml;
        $('playerWaitPoints').innerHTML = pointsHtml;
        $('playerWaitMsg').textContent = 'Waiting for other teams...';
      } else if (!voteReady) {
        show('playerReady');
      } else {
        show('playerVote');
      }
      break;
    case 'results':
    case 'roundEnd':
      show('playerDone');
      $('playerDoneBanner').innerHTML = bannerHtml;
      $('playerDonePoints').innerHTML = pointsHtml;
      $('playerDoneMsg').textContent = state.phase === 'roundEnd' ? 'Round finished! Waiting for next round...' : 'Results being revealed on the big screen...';
      break;
    default:
      show('playerWaiting');
      $('playerSlotBanner').innerHTML = bannerHtml;
      $('playerWaitPoints').innerHTML = pointsHtml;
      $('playerWaitMsg').textContent = 'Waiting...';
  }
}

function renderSlotSelect() {
  const slots = state.slots.filter(s => s.playerIds.length > 0);
  $('slotSelectGrid').innerHTML = slots.map(s => `
    <div class="team-select-btn ${s.connected ? 'taken' : ''}" onclick="${s.connected ? '' : "socket.emit('join-slot', " + s.id + ")"}">
      ${esc(s.name)}
      <span class="type-badge" style="display:block;font-size:0.7rem;letter-spacing:2px;text-transform:uppercase;margin-top:6px">${s.type}</span>
    </div>
  `).join('');
}

function beginVoting(el) {
  playSound('startSound');
  el.classList.add('red-glow');
  setTimeout(() => {
    el.classList.remove('red-glow');
    const o = $('screenOverlay');
    o.style.opacity = '1';
    setTimeout(() => {
      o.style.opacity = '0';
      voteReady = true;
      render();
    }, 350);
  }, 1200);
}

function sendVote(el, choice) {
  playSound('voteSelectSound');
  el.classList.add('red-glow');
  setTimeout(() => {
    el.classList.remove('red-glow');
    const o = $('screenOverlay');
    o.style.opacity = '1';
    setTimeout(() => {
      o.style.opacity = '0';
      socket.emit('vote', choice);
    }, 350);
  }, 1200);
}

function toggleFullscreen() {
  if (!document.fullscreenElement) {
    document.documentElement.requestFullscreen().catch(() => {});
  } else {
    document.exitFullscreen().catch(() => {});
  }
}

// ---------- Host Music Player ----------
const MUSIC_TRACKS = [
  { file: 'music/1-06. Unary Game.mp3', label: 'Unary Game (999)' },
  { file: 'music/3-02. Ambidexterity.mp3', label: 'Ambidexterity (VLR)' },
  { file: 'music/4-01. Sinisterness.mp3', label: 'Sinisterness (VLR)' },
  { file: 'music/4-03. Eeriness.mp3', label: 'Eeriness (VLR)' },
  { file: 'music/4-04. Strain.mp3', label: 'Strain (VLR)' },
  { file: 'music/4-07. Anxiousness.mp3', label: 'Anxiousness (VLR)' },
];

function initMusicPlayer() {
  const sel = $('musicSelect');
  if (!sel) return;
  sel.innerHTML = '<option value="">♫ Select track</option>' +
    MUSIC_TRACKS.map((t, i) => `<option value="${i}">${t.label}</option>`).join('');
  sel.addEventListener('change', onMusicSelect);
  showMusicPlayer(true);
}

function onMusicSelect() {
  const sel = $('musicSelect');
  const audio = $('hostMusic');
  const btn = $('musicToggle');
  if (!sel || !audio || !btn) return;
  const idx = parseInt(sel.value);
  if (isNaN(idx)) {
    audio.pause();
    audio.src = '';
    btn.textContent = '▶';
    return;
  }
  audio.src = '/' + MUSIC_TRACKS[idx].file;
  audio.loop = true;
  audio.play().catch(() => {});
  btn.textContent = '■';
}

function toggleMusic() {
  const audio = $('hostMusic');
  const btn = $('musicToggle');
  if (!audio || !btn) return;
  if (audio.paused) {
    if (!audio.src) {
      const sel = $('musicSelect');
      if (sel && sel.value) { sel.dispatchEvent(new Event('change')); return; }
      return;
    }
    audio.play().catch(() => {});
    btn.textContent = '■';
  } else {
    audio.pause();
    btn.textContent = '▶';
  }
}

function showMusicPlayer(show) {
  const el = $('musicPlayer');
  if (el) el.style.display = show ? 'flex' : 'none';
}
