#!/bin/bash
echo "=== 卸载 TF 卡 ==="
ssh -i /root/.ssh/k2 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.110.43 '
umount /mnt/sdcard/mmcblk1p1 2>/dev/null
echo "卸载完成"
echo "确认 TF 卡设备:"
ls -la /dev/mmcblk1
echo "UDISK 可用空间:"
df -h /mnt/UDISK | tail -1
'
