#!/bin/bash
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
LO=$(losetup -f --show -o $((112262*512)) --sizelimit $((35200*512)) "$OR")
echo "=== boot(p4) header hexdump (first 128 bytes) ==="
dd if="$LO" bs=1 count=128 2>/dev/null | od -A x -t x1z --width=16
echo ""
echo "=== strings in first 4KB (looking for cmdline) ==="
dd if="$LO" bs=1 count=4096 2>/dev/null | strings -n 4 | head -40
echo ""
echo "=== full header 1KB as ascii ==="
dd if="$LO" bs=1 count=1024 2>/dev/null | strings -n 3
losetup -d "$LO"

echo ""
echo "=== env partition (p2) strings ==="
LO2=$(losetup -f --show -o $((108166*512)) --sizelimit $((2048*512)) "$OR")
dd if="$LO2" bs=1 2>/dev/null | strings -n 4 | head -40
losetup -d "$LO2"
