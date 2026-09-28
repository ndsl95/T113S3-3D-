#!/bin/bash
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
LO=$(losetup -f --show -o $((100845574*512)) --sizelimit $((21661656*512)) "$OR")

for off in 65536 1048576 1048592 2097152; do
  echo "=== UDISK @ $off (256 bytes) ==="
  dd if="$LO" bs=1 skip=$off count=256 2>/dev/null | od -A x -t x1z --width=16
  echo ""
done

echo "=== unique 4-byte values in first 4KB after 64KB (entropy hint) ==="
dd if="$LO" bs=1 skip=65536 count=4096 2>/dev/null | od -A n -t x4 -v | tr -s ' ' '\n' | sort -u | wc -l

losetup -d "$LO"
