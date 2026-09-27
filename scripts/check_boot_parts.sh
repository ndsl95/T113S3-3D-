#!/bin/bash
SM=/mnt/h/disk_small.img
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
cmp_part() {
  name=$1; start=$2; cnt=$3
  a=$(dd if=$SM bs=512 skip=$start count=$cnt 2>/dev/null | md5sum | cut -d' ' -f1)
  b=$(dd if=$OR bs=512 skip=$start count=$cnt 2>/dev/null | md5sum | cut -d' ' -f1)
  if [ "$a" = "$b" ]; then echo "$name: MATCH"; else echo "$name: DIFFER  small=$a  orig=$b"; fi
}
cmp_part "head(0-73727 incl bootloader+GPT)" 0 73728
cmp_part "p1_boot-resource" 73728 34438
cmp_part "p2_env" 108166 2048
cmp_part "p3_env-redund" 110214 2048
cmp_part "p4_boot" 112262 35200
cmp_part "p5_rootfs first 1MB" 147462 2048
