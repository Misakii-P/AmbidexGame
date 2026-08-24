const fs = require('fs');
const path = require('path');

const pngPath = path.join(__dirname, '..', 'public', 'icons', 'icon.png');
const icoPath = path.join(__dirname, '..', 'public', 'icons', 'icon.ico');

const png = fs.readFileSync(pngPath);
const size = png.length;

// ICO header
const buf = Buffer.alloc(22 + size);
buf.writeUInt16LE(0, 0);   // reserved
buf.writeUInt16LE(1, 2);   // type: ICO
buf.writeUInt16LE(1, 4);   // count: 1 image

// Directory entry
buf.writeUInt8(0, 6);      // width: 0 = 256
buf.writeUInt8(0, 7);      // height: 0 = 256
buf.writeUInt8(0, 8);      // color count
buf.writeUInt8(0, 9);      // reserved
buf.writeUInt16LE(1, 10);  // planes
buf.writeUInt16LE(32, 12); // bits per pixel
buf.writeUInt32LE(size, 14); // image size
buf.writeUInt32LE(22, 18); // offset

// Image data (raw PNG)
png.copy(buf, 22);

fs.writeFileSync(icoPath, buf);
console.log(`Created ${icoPath} (${buf.length} bytes)`);
