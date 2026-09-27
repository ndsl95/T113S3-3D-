#!/bin/bash
OR="/mnt/g/飞牛分享文件/全志T113原系统刷机包/backup/disk.img"
echo "=== LBA0 MBR (offset 0) ==="
xxd -s 0 -l 512 "$OR" | head -4
echo ""
echo "=== LBA1 (offset 512) - GPT header? ==="
xxd -s 512 -l 96 "$OR"
echo ""
echo "=== LBA2 (offset 1024) - GPT entry? ==="
xxd -s 1024 -l 128 "$OR"
echo ""
echo "=== offset 8192 (boot0 eGON?) ==="
xxd -s 8176 -l 128 "$OR"
echo ""
echo "=== search 'EFI PART' in first 1MB ==="
dd if="$OR" bs=512 count=2048 2>/dev/null | grep -abo "EFI PART" | head
echo ""
echo "=== search 'eGON' in first 1MB ==="
dd if="$OR" bs=512 count=2048 2>/dev/null | grep -abo "eGON" | head
