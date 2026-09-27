#!/bin/bash
ssh -i /root/.ssh/k2 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.110.43 '
echo "=== TF卡状态 ==="
mount | grep mmcblk1 || echo "TF卡未挂载"
ls -la /dev/mmcblk1
echo
echo "=== 卸载TF卡 ==="
umount /dev/mmcblk1p1 2>/dev/null
umount /mnt/sdcard/mmcblk1p1 2>/dev/null
echo "已卸载"
echo
echo "=== 断开当前USB gadget ==="
echo "" > /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null
echo "UDC已断开"
echo
echo "=== 加载 g_mass_storage ==="
modprobe g_mass_storage file=/dev/mmcblk1 removable=1 ro=0 stall=0 iSerialNumber=1234567890 2>&1
echo
echo "=== 检查模块 ==="
lsmod | grep mass_storage
'
