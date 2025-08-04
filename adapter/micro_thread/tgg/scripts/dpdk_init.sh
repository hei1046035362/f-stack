# 分配大页  根据实际情况调整大小
echo 1536 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
mkdir /mnt/huge
mount -t hugetlbfs nodev /mnt/huge
echo 0 > /proc/sys/kernel/randomize_va_space

# 加载驱动
modprobe uio
insmod /usr/src/linux-headers-$(uname -r)/extra/dpdk/igb_uio.ko
insmod /usr/lib/modules/$(uname -r)/extra/dpdk/rte_kni.ko carrier=on

# 网口驱动更换(内核驱动换igb_uio)  
### 注：ens6 要替换为dpdk要绑定的网口名
eth1_pci=$(/usr/local/bin/dpdk-devbind.py -s|grep "ens6"|awk '{print $1}')
ifconfig ens6 down
/usr/local/bin/dpdk-devbind.py -u ens6
/usr/local/bin/dpdk-devbind.py -b igb_uio $eth1_pci
