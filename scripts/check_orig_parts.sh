#!/bin/bash
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
ZERO=$(dd if=/dev/zero bs=1M count=1 2>/dev/null | md5sum | cut -d' ' -f1)

echo "=== private (p7): start=100812806 size=32768 sectors (16MB) ==="
LO=$(losetup -f --show -o $((100812806*512)) --sizelimit $((32768*512)) "$OR")
blkid "$LO" 2>&1
for mb in 0 4 8 15; do
  d=$(dd if="$LO" bs=1M skip=$mb count=1 2>/dev/null | md5sum | cut -d' ' -f1)
  [ "$d" = "$ZERO" ] && echo "  ${mb}MB: zeros" || echo "  ${mb}MB: NON-ZERO"
done
losetup -d "$LO"

echo ""
echo "=== UDISK (p8): start=100845574 size=21661656 sectors (10.3GB) ==="
LO=$(losetup -f --show -o $((100845574*512)) --sizelimit $((21661656*512)) "$OR")
blkid "$LO" 2>&1
for mb in 0 1 10 100 500 1000 3000 6000 9000 10200; do
  d=$(dd if="$LO" bs=1M skip=$mb count=1 2>/dev/null | md5sum | cut -d' ' -f1)
  [ "$d" = "$ZERO" ] && echo "  ${mb}MB: zeros" || echo "  ${mb}MB: NON-ZERO"
done
losetup -d "$LO"

echo ""
echo "=== boot (p4): start=112262 size=35200 sectors (17.2MB) ==="
LO=$(losetup -f --show -o $((112262*512)) --sizelimit $((35200*512)) "$OR")
blkid "$LO" 2>&1
echo "first 32 bytes:"; dd if="$LO" bs=1 count=32 2>/dev/null | od -A n -t x1
losetup -d "$LO"
