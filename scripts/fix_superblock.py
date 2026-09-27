#!/usr/bin/env python3
import struct
import sys

img_path = "/mnt/h/disk_small.img"
rootfs_offset = 147462 * 512  # 75500544

# 新的值
new_blocks_count = 1048576    # 0x100000 (4GB / 4096)
new_inodes_count = 262144     # 0x40000

block_size = 4096
blocks_per_group = 32768

# 备份超级块的块组号 (sparse_super: 0, 1, 3, 5, 7, ...)
backup_bgs = [1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31]

with open(img_path, "r+b") as f:
    # 修复主超级块 (块 0)
    sb_offset = rootfs_offset + 1024  # 超级块从分区偏移 1024 开始

    # 读取当前 s_inodes_count
    f.seek(sb_offset)
    inodes_count = struct.unpack("<I", f.read(4))[0]
    f.seek(sb_offset + 4)
    blocks_count = struct.unpack("<I", f.read(4))[0]
    print(f"Primary superblock: inodes={inodes_count} blocks={blocks_count}")

    # 修改 s_inodes_count
    f.seek(sb_offset)
    f.write(struct.pack("<I", new_inodes_count))
    print(f"  -> Updated inodes_count to {new_inodes_count}")

    # 修复备份超级块
    for bg in backup_bgs:
        block_num = bg * blocks_per_group
        offset = rootfs_offset + block_num * block_size

        # 检查魔数
        f.seek(offset + 56)
        magic = f.read(2)
        if magic != b'\x53\xef':
            continue

        # 读取当前值
        f.seek(offset)
        cur_inodes = struct.unpack("<I", f.read(4))[0]
        f.seek(offset + 4)
        cur_blocks = struct.unpack("<I", f.read(4))[0]

        # 修改 s_inodes_count 和 s_blocks_count
        f.seek(offset)
        f.write(struct.pack("<I", new_inodes_count))
        f.seek(offset + 4)
        f.write(struct.pack("<I", new_blocks_count))

        print(f"Backup superblock BG{bg} (block {block_num}): inodes {cur_inodes}->{new_inodes_count}, blocks {cur_blocks}->{new_blocks_count}")

    f.flush()

print("\nDone! Superblocks updated.")
