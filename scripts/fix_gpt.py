import struct, zlib, os

IMG = "/mnt/h/disk_small.img"
SECTOR = 512
NENT = 8
ESZ = 128

f = open(IMG, "r+b")

# --- read current primary header + entries ---
f.seek(512)
hdr = bytearray(f.read(92))
entries_lba = struct.unpack("<Q", hdr[72:80])[0]
disk_guid = bytes(hdr[56:72])

f.seek(entries_lba * SECTOR)
entries = bytearray(f.read(NENT * ESZ))

# --- find last partition end ---
last_end = 0
for i in range(NENT):
    e = entries[i*ESZ:(i+1)*ESZ]
    if e[0:16] == b"\x00"*16:
        continue
    s, en = struct.unpack("<QQ", e[32:48])
    last_end = max(last_end, en)
print("entries_lba =", entries_lba)
print("last partition end (LBA) =", last_end)

# --- new total: leave 33 sectors after last partition for backup GPT ---
total = last_end + 1 + 33
need = total * SECTOR
if os.path.getsize(IMG) < need:
    f.truncate(need)
    print("extended image to", os.path.getsize(IMG), "bytes /", total, "sectors")

alt = total - 1
first_usable = 73728
last_usable = alt - 3

entries_crc = zlib.crc32(bytes(entries)) & 0xffffffff

def make_header(my_lba, alt_lba, ent_lba, first, last):
    h = bytearray(512)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)
    struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<I", h, 16, 0)
    struct.pack_into("<I", h, 20, 0)
    struct.pack_into("<Q", h, 24, my_lba)
    struct.pack_into("<Q", h, 32, alt_lba)
    struct.pack_into("<Q", h, 40, first)
    struct.pack_into("<Q", h, 48, last)
    h[56:72] = disk_guid
    struct.pack_into("<Q", h, 72, ent_lba)
    struct.pack_into("<I", h, 80, NENT)
    struct.pack_into("<I", h, 84, ESZ)
    struct.pack_into("<I", h, 88, entries_crc)
    crc = zlib.crc32(bytes(h[0:92])) & 0xffffffff
    struct.pack_into("<I", h, 16, crc)
    return h

# primary header (LBA1) + entries at entries_lba and at LBA2 (两处副本，与原盘一致)
ph = make_header(1, alt, entries_lba, first_usable, last_usable)
f.seek(512); f.write(ph)
f.seek(entries_lba * SECTOR); f.write(entries)
f.seek(2 * SECTOR); f.write(entries)

# backup: entries right before backup header
backup_entries_lba = alt - (NENT * ESZ) // SECTOR
f.seek(backup_entries_lba * SECTOR); f.write(entries)
bh = make_header(alt, 1, backup_entries_lba, first_usable, last_usable)
f.seek(alt * SECTOR); f.write(bh)

f.flush()
os.fsync(f.fileno())
f.close()

print("primary : last_usable =", last_usable, " alt =", alt)
print("backup  : entries @", backup_entries_lba, " header @", alt)
print("done")
