import struct, zlib

def check(path, label):
    print(f"===== {label}: {path} =====")
    with open(path, "rb") as f:
        f.seek(512)
        h = f.read(92)
        if h[0:8] != b"EFI PART":
            print("  no GPT header!"); return
        hdr_crc_stored = struct.unpack("<I", h[16:20])[0]
        my_lba, alt = struct.unpack("<QQ", h[24:40])
        first, last = struct.unpack("<QQ", h[40:56])
        ent_lba, nent, esz, ent_crc_stored = struct.unpack("<QIII", h[72:92])
        # recompute header crc with crc field zeroed
        hz = bytearray(h); hz[16:20] = b"\x00\x00\x00\x00"
        hdr_crc_calc = zlib.crc32(bytes(hz)) & 0xffffffff
        # recompute entries crc
        f.seek(ent_lba*512)
        ent = f.read(nent*esz)
        ent_crc_calc = zlib.crc32(ent) & 0xffffffff
        print(f"  my_lba={my_lba} alt={alt} first={first} last={last}")
        print(f"  entries_lba={ent_lba} n={nent} esz={esz}")
        print(f"  header CRC  stored={hdr_crc_stored:08X} calc={hdr_crc_calc:08X}  -> {'OK' if hdr_crc_stored==hdr_crc_calc else 'MISMATCH <<< BAD'}")
        print(f"  entries CRC stored={ent_crc_stored:08X} calc={ent_crc_calc:08X}  -> {'OK' if ent_crc_stored==ent_crc_calc else 'MISMATCH <<< BAD'}")

check("/mnt/g/飞牛分享文件/全志T113原系统刷机包/disk_small.img", "旧(有问题)镜像 G:")
check("/mnt/h/disk_small.img", "修复后镜像 H:")
check("/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img", "原始镜像")
