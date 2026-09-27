#!/bin/bash
ssh -i /root/.ssh/k2 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 root@192.168.110.43 '
echo "===系统==="
uname -a
echo
echo "===存储设备==="
cat /proc/partitions
echo
echo "===mmc/sd设备==="
ls -la /dev/mmcblk* /dev/sd* 2>/dev/null
echo
echo "===挂载==="
mount | grep -E "mmc|sd"
echo
echo "===eMMC剩余空间==="
df -h | grep -E "mmc|sd|Filesystem"
'
