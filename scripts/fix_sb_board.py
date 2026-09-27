#!/usr/bin/env python3
import struct
import os

dev = "/dev/mmcblk1"
rootfs_offset = 147462 * 512  # 75500544

new_blocks_count = 1048576
new_inodes_count = 262144

block_size = 4096
blocks_per_group = 32768
backup_bgs = [1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31]

with open(dev, "r+b") as f:
    # 主超级块
    sb_offset = rootfs_offset + 1024
    f.seek(sb_offset)
    inodes_count = struct.unpack("<I", f.read(4))[0]
    f.seek(sb_offset + 4)
    blocks_count = struct.unpack("<I", f.read(4))[0]
    print(f"Primary: inodes={inodes_count} blocks={blocks_count}")

    f.seek(sb_offset)
    f.write(struct.pack("<I", new_inodes_count))
    print(f"  -> inodes_count -> {new_inodes_count}")

    # 备份超级块
    for bg in backup_bgs:
        block_num = bg * blocks_per_group
        offset = rootfs_offset + block_num * block_size

        f.seek(offset + 56)
        magic = f.read(2)
        if magic != b'\x53\xef':
            continue

        f.seek(offset)
        cur_inodes = struct.unpack("<I", f.read(4))[0]
        f.seek(offset + 4)
        cur_blocks = struct.unpack("<I", f.read(4))[0]

        f.seek(offset)
        f.write(struct.pack("<I", new_inodes_count))
        f.seek(offset + 4)
        f.write(struct.pack("<I", new_blocks_count))

        print(f"BG{bg}: inodes {cur_inodes}->{new_inodes_count}, blocks {cur_blocks}->{new_blocks_count}")

    f.flush()
    os.fsync(f.fileno())

print("Done!")
