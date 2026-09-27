import struct
SM = "/mnt/h/disk_small.img"
OR = "/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"

def read_header(f, off):
    f.seek(off)
    h = f.read(92)
    if h[:8] != b"EFI PART":
        return None
    hsz, hcrc = struct.unpack("<II", h[12:20])
    mylba, altlba, first, last = struct.unpack("<QQQQ", h[24:56])
    entries_lba, nent, esz, ecrc = struct.unpack("<QIII", h[72:92])
    return dict(mylba=mylba, alt=altlba, first=first, last=last,
                entries_lba=entries_lba, nent=nent, esz=esz)

def read_entries(f, lba, nent, esz):
    f.seek(lba*512)
    data = f.read(nent*esz)
    out = []
    for i in range(nent):
        e = data[i*esz:(i+1)*esz]
        if e[0:16] == b"\x00"*16:
            continue
        start, end = struct.unpack("<QQ", e[32:48])
        name = e[56:128].decode("utf-16-le").rstrip("\x00")
        out.append((i+1, start, end, name))
    return out

def md5_region(path, off, length):
    import hashlib
    h = hashlib.md5()
    with open(path, "rb") as f:
        f.seek(off)
        h.update(f.read(length))
    return h.hexdigest()

print("=== boot0 region (offset 8192, 56KB) ===")
print("  small:", md5_region(SM, 8192, 56*1024))
print("  orig :", md5_region(OR, 8192, 56*1024))
print()
print("=== LBA0 (offset 0, 512B) ===")
print("  small:", md5_region(SM, 0, 512))
print("  orig :", md5_region(OR, 0, 512))
print()

for label, path in [("SMALL", SM), ("ORIG", OR)]:
    print(f"========== {label} ==========")
    with open(path, "rb") as f:
        h = read_header(f, 512)
        print("Header LBA1:", h)
        if h:
            print("entries @%d:" % h["entries_lba"], read_entries(f, h["entries_lba"], h["nent"], h["esz"]))
            print("entries @LBA2:", read_entries(f, 2, h["nent"], h["esz"]))
    print()
