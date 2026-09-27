#!/bin/bash
SM=/mnt/h/disk_small.img
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
LOGF="/mnt/h/T113S3 铝合金3D打印一体机/rebuild.log"
exec >"$LOGF" 2>&1

echo "=== START $(date) ==="

echo "--- step1: resize original rootfs 48G -> 4G ---"
LO=$(losetup -f --show -o $((147462*512)) --sizelimit $((100663296*512)) "$OR")
echo "loop=$LO"
echo "e2fsck before:"
e2fsck -f -y "$LO"
echo "resize2fs 48G -> 4G ..."
resize2fs "$LO" 4194304K
echo "e2fsck after:"
e2fsck -f -y "$LO"
echo "final fs info:"
dumpe2fs -h "$LO" 2>&1 | grep -E "Block count|Inode count|Block size|Free blocks"
losetup -d "$LO"

echo ""
echo "--- step2: copy partitions into small image ---"
echo "rootfs (4G) 147462.. -> .."
dd if="$OR" of="$SM" bs=1M iflag=skip_bytes,count_bytes skip=$((147462*512)) count=$((8388608*512)) oflag=seek_bytes seek=$((147462*512)) conv=notrunc status=progress
echo "dsp0 ..."
dd if="$OR" of="$SM" bs=1M iflag=skip_bytes,count_bytes skip=$((100810758*512)) count=$((2048*512)) oflag=seek_bytes seek=$((8536070*512)) conv=notrunc status=progress
echo "private ..."
dd if="$OR" of="$SM" bs=1M iflag=skip_bytes,count_bytes skip=$((100812806*512)) count=$((32768*512)) oflag=seek_bytes seek=$((8538118*512)) conv=notrunc status=progress
echo "UDISK (zero, 2GB) ..."
dd if=/dev/zero of="$SM" bs=1M count=2048 oflag=seek_bytes seek=$((8570886*512)) conv=notrunc status=progress
sync
echo ""
echo "=== DONE $(date) ==="
