import struct
f = open('/dev/mmcblk1', 'rb')
rootfs = 147462 * 512
bs = 4096
# Check all odd block groups up to 31
for bg in range(1, 32, 2):
    off = rootfs + bg * 32768 * bs
    f.seek(off + 56)
    magic = f.read(2)
    if magic == b'\x53\xef':
        f.seek(off)
        inodes = struct.unpack('<I', f.read(4))[0]
        f.seek(off + 4)
        blocks = struct.unpack('<I', f.read(4))[0]
        print(f'BG{bg}: inodes={inodes} blocks={blocks}')
    else:
        print(f'BG{bg}: no superblock')
f.close()
