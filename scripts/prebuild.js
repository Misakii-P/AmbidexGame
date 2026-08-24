const path = require('path');
const os = require('os');
const fs = require('fs');

const nexeCachePath = path.join(os.homedir(), '.nexe', 'windows-x64-14.15.3');
const iconPath = path.join(__dirname, '..', 'public', 'icons', 'icon-small.ico');

if (!fs.existsSync(nexeCachePath)) {
  console.log('Nexe cache not found, skipping icon patch');
  process.exit(0);
}

const { rcedit } = require('rcedit');
rcedit(nexeCachePath, { icon: iconPath }).then(() => {
  console.log('Icon patched in nexe cache');
}).catch((err) => {
  console.log('Failed to patch icon:', err.message);
});