# PGM (P5) -> PNG с уменьшением в N раз: python3 -I pgm2png.py IN.pgm OUT.png [N]
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]; k = int(sys.argv[3]) if len(sys.argv) > 3 else 2
data = open(src, 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
ow, oh = w // k, h // k
rows = []
for y in range(oh):
    r = px[(y * k) * w:(y * k) * w + w]
    rows.append(b'\x00' + bytes(r[x * k] for x in range(ow)))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(dst, 'wb').write(png)
