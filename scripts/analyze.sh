#!/bin/bash
echo "=== Disk space ==="
df -h /mnt/g /mnt/h

echo ""
echo "=== backup.zip contents (first 25) ==="
unzip -l "/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup.zip" 2>&1 | head -25

echo ""
echo "=== Original rootfs (partition 5) filesystem info ==="
OFF=$((147462*512))
LO=$(losetup -f --show -o $OFF "/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img")
dumpe2fs -h "$LO" 2>&1 | grep -E "Block count|Free blocks|Inode count|Free inodes|Block size|Reserved block count|Filesystem state"
echo "--- resize2fs minimum size ---"
resize2fs -P "$LO" 2>&1
losetup -d "$LO"
