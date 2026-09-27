#!/bin/bash
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
check() {
  name=$1; start=$2; cnt=$3
  echo "=== $name (start=$start count=$cnt sectors) ==="
  LO=$(losetup -f --show -o $((start*512)) --sizelimit $((cnt*512)) "$OR" 2>&1)
  blkid "$LO" 2>&1
  if dumpe2fs -h "$LO" >/dev/null 2>&1; then
    dumpe2fs -h "$LO" 2>&1 | grep -E "Block count|Free blocks|Inode count|Free inodes|Block size"
    mkdir -p /tmp/pp
    mount -o ro "$LO" /tmp/pp 2>&1 && { echo "--- mount OK ---"; df -h /tmp/pp; ls /tmp/pp | head -10; umount /tmp/pp; }
  else
    echo "(not ext2/3/4) first bytes:"
    dd if="$LO" bs=1 count=16 2>/dev/null | xxd
  fi
  losetup -d "$LO" 2>/dev/null
}
check dsp0 100810758 2048
check private 100812806 32768
check UDISK 100845574 21661656
