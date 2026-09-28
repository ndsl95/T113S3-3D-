#!/bin/bash
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
SM="/mnt/h/disk_small.img"
echo "=== compare first 75MB (reserved 0-36MB + p1..p4), 1MB blocks ==="
for i in $(seq 0 74); do
  a=$(dd if="$OR" bs=1M skip=$i count=1 2>/dev/null | md5sum | cut -d' ' -f1)
  b=$(dd if="$SM" bs=1M skip=$i count=1 2>/dev/null | md5sum | cut -d' ' -f1)
  if [ "$a" != "$b" ]; then echo "DIFF @ ${i}MB"; fi
done
echo "=== done ==="
