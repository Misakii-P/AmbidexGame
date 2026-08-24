const fs = require('fs');
const path = require('path');
const { rcedit } = require('rcedit');

const exePath = path.join(__dirname, '..', 'dist', 'AmbidexGame.exe');
const icoPath = path.join(__dirname, '..', 'public', 'icons', 'icon.ico');

// Wait for pkg to release the file
setTimeout(async () => {
  try {
    await rcedit(exePath, { icon: icoPath });
    console.log('Icon applied');
  } catch (err) {
    console.error('Icon failed:', err.message);
  }

  // Patch to GUI subsystem
  const buf = fs.readFileSync(exePath);
  const peOffset = buf.readUInt32LE(0x3C);
  const magic = buf.toString('utf8', peOffset, peOffset + 2);
  if (magic !== 'PE') { console.error('Invalid PE signature'); process.exit(1); }
  const subsysOffset = peOffset + 24 + 68;
  const old = buf.readUInt16LE(subsysOffset);
  console.log(`Subsystem: ${old} (${old === 3 ? 'CONSOLE' : 'GUI'})`);
  if (old !== 2) {
    buf.writeUInt16LE(2, subsysOffset);
    fs.writeFileSync(exePath, buf);
    console.log('Patched to GUI subsystem');
  }
}, 500);
