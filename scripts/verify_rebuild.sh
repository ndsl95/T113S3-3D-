#!/bin/bash
SM=/mnt/h/disk_small.img
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"

echo "=== fdisk small image ==="
fdisk -l "$SM" 2>&1 | tail -14

echo ""
echo "=== rootfs e2fsck -n (small) ==="
LO=$(losetup -f --show -o $((147462*512)) --sizelimit $((8388608*512)) "$SM")
e2fsck -f -n "$LO" 2>&1 | tail -8
echo "--- mount + contents ---"
mkdir -p /tmp/s
mount -o ro "$LO" /tmp/s 2>&1 && {
  echo "MOUNT OK"
  df -h /tmp/s
  echo "--- top level ---"; ls /tmp/s
  echo "--- critical ---"; ls -la /tmp/s/init /tmp/s/sbin/init /tmp/s/bin/busybox 2>&1
}
umount /tmp/s 2>/dev/null
losetup -d "$LO"

echo ""
echo "=== dsp0 md5 (small vs orig) ==="
echo -n "  small: "; dd if="$SM" bs=512 skip=8536070 count=2048 2>/dev/null | md5sum
echo -n "  orig : "; dd if="$OR" bs=512 skip=100810758 count=2048 2>/dev/null | md5sum

echo ""
echo "=== rootfs file-count: small vs orig ==="
LO2=$(losetup -f --show -o $((147462*512)) --sizelimit $((8388608*512)) "$OR")
mkdir -p /tmp/s /tmp/o
mount -o ro "$SM" /tmp/s 2>/dev/null || mount -o ro $(losetup -f --show -o $((147462*512)) --sizelimit $((8388608*512)) "$SM") /tmp/s
mount -o ro "$LO2" /tmp/o
echo -n "  small files: "; find /tmp/s 2>/dev/null | wc -l
echo -n "  orig  files: "; find /tmp/o 2>/dev/null | wc -l
echo "--- diff (only in orig) ---"
diff <(cd /tmp/o && find . | sort) <(cd /tmp/s && find . | sort) | grep '^<' | head -20
umount /tmp/s /tmp/o 2>/dev/null
losetup -d "$LO2" 2>/dev/null
