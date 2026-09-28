#!/bin/bash
A=/mnt/h/disk_full.img
B="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
echo -n "full  p8 start      : "; dd if="$A" bs=512 skip=8570886 count=4096 2>/dev/null | md5sum
echo -n "orig  p8 start      : "; dd if="$B" bs=512 skip=100845574 count=4096 2>/dev/null | md5sum
echo -n "full  p8 +5GB       : "; dd if="$A" bs=512 skip=$((8570886+10485760)) count=4096 2>/dev/null | md5sum
echo -n "orig  p8 +5GB       : "; dd if="$B" bs=512 skip=$((100845574+10485760)) count=4096 2>/dev/null | md5sum
echo -n "full  p8 +10GB      : "; dd if="$A" bs=512 skip=$((8570886+20971520)) count=4096 2>/dev/null | md5sum
echo -n "orig  p8 +10GB      : "; dd if="$B" bs=512 skip=$((100845574+20971520)) count=4096 2>/dev/null | md5sum
