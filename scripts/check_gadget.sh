#!/bin/bash
ssh -i /root/.ssh/k2 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.110.43 '
echo "===当前USB gadget配置==="
find /sys/kernel/config/usb_gadget/g1 -type f 2>/dev/null | head -50
echo
echo "===functions==="
ls /sys/kernel/config/usb_gadget/g1/functions/ 2>/dev/null
echo
echo "===configs==="
ls /sys/kernel/config/usb_gadget/g1/configs/ 2>/dev/null
echo
echo "===UDC==="
cat /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null
echo
echo "===检查可用模块==="
find /lib/modules -name "g_mass_storage*" 2>/dev/null
find /lib/modules -name "usb_f_mass_storage*" 2>/dev/null
find /lib/modules -name "libcomposite*" 2>/dev/null
echo
echo "===检查modprobe==="
which modprobe 2>/dev/null || echo "no modprobe"
'
