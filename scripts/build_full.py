import struct, zlib, os, shutil

OR = "/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
SM = "/mnt/h/disk_small.img"
OUT = "/mnt/h/disk_full.img"
SECTOR = 512
NENT, ESZ = 8, 128

UDISK_SRC_OFF = 100845574 * SECTOR
UDISK_SIZE = 21661656 * SECTOR
UDISK_DST_OFF = 8570886 * SECTOR
UDISK_DST_LBA = 8570886
DISK_END = UDISK_DST_LBA + 21661656 - 1
TOTAL = DISK_END + 1 + 33

print("1) copy small -> full")
shutil.copyfile(SM, OUT)

print("2) write full UDISK content (%.2f GB)" % (UDISK_SIZE/1073741824))
with open(OR, "rb") as fi, open(OUT, "r+b") as fo:
    fi.seek(UDISK_SRC_OFF)
    fo.seek(UDISK_DST_OFF)
    remaining = UDISK_SIZE
    done = 0
    while remaining > 0:
        chunk = fi.read(min(8*1024*1024, remaining))
        if not chunk:
            break
        fo.write(chunk)
        remaining -= len(chunk)
        done += len(chunk)

print("3) update p8 entry + rebuild GPT")
with open(OUT, "r+b") as f:
    f.seek(512)
    hdr = bytearray(f.read(92))
    ent_lba = struct.unpack("<Q", hdr[72:80])[0]
    disk_guid = bytes(hdr[56:72])
    f.seek(ent_lba * SECTOR)
    entries = bytearray(f.read(NENT*ESZ))

    for i in range(NENT):
        e = entries[i*ESZ:(i+1)*ESZ]
        if e[0:16] == b"\x00"*16:
            continue
        s, en = struct.unpack("<QQ", e[32:48])
        if s == UDISK_DST_LBA:
            struct.pack_into("<QQ", e, 32, s, DISK_END)
            entries[i*ESZ:(i+1)*ESZ] = e
            print("   p8 end ->", DISK_END)

    need = TOTAL * SECTOR
    if os.path.getsize(OUT) < need:
        f.truncate(need)
    alt = TOTAL - 1
    last_usable = alt - 3
    ent_crc = zlib.crc32(bytes(entries)) & 0xffffffff

    def mk(my, altl, elba, first, last):
        h = bytearray(512)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)
        struct.pack_into("<I", h, 12, 92)
        struct.pack_into("<I", h, 16, 0)
        struct.pack_into("<I", h, 20, 0)
        struct.pack_into("<Q", h, 24, my)
        struct.pack_into("<Q", h, 32, altl)
        struct.pack_into("<Q", h, 40, first)
        struct.pack_into("<Q", h, 48, last)
        h[56:72] = disk_guid
        struct.pack_into("<Q", h, 72, elba)
        struct.pack_into("<I", h, 80, NENT)
        struct.pack_into("<I", h, 84, ESZ)
        struct.pack_into("<I", h, 88, ent_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[0:92])) & 0xffffffff)
        return h

    f.seek(512); f.write(mk(1, alt, ent_lba, 73728, last_usable))
    f.seek(ent_lba*SECTOR); f.write(entries)
    f.seek(2*SECTOR); f.write(entries)
    be = alt - (NENT*ESZ)//SECTOR
    f.seek(be*SECTOR); f.write(entries)
    f.seek(alt*SECTOR); f.write(mk(alt, 1, be, 73728, last_usable))
    f.flush(); os.fsync(f.fileno())

print("TOTAL =", TOTAL, "sectors =", TOTAL*SECTOR, "bytes")
