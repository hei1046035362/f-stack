# 分配大页  根据实际情况调整大小

# 获取CPU核心数（物理核心）
get_cpu_cores() {
    # if command -v lscpu &> /dev/null; then
    #     lscpu | awk -F: '/^CPU\(s\):/ {gsub(/[^0-9]/, "", $2); print $2}' 
    # else
    grep -c "cpu cores" /proc/cpuinfo
    # fi
}

# 计算大页页数
calc_hugepages() {
    local cores=$(get_cpu_cores)  # 核数
    echo $(( cores * 5 / 4 * 512 ))                    # 转换为2MB页数
}

# 设置大页内存
set_hugepages() {
    local cur_huge=$(calc_hugepages)
    local prev_huges=$(cat /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages)
    if [ "$cur_huge" -ne "$prev_huges" ]; then
        echo "大页内存($prev_huges)不够即将设置为 $cur_huge"
        echo $cur_huge > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
        mkdir /mnt/huge
        mount -t hugetlbfs nodev /mnt/huge
        echo 0 > /proc/sys/kernel/randomize_va_space
    fi
}

# 加载驱动
insmod_dpdk_io() {
    if lsmod | grep -q "^igb_uio"; then
        echo "igb_uio 模块已加载"
    else
        echo "igb_uio 驱动尝试加载..."
        # 加载驱动
        modprobe uio
        insmod /usr/src/linux-headers-$(uname -r)/extra/dpdk/igb_uio.ko
        insmod /usr/lib/modules/$(uname -r)/extra/dpdk/rte_kni.ko carrier=on
    fi
}
# dpdk网口绑定(内核驱动换igb_uio)  
bind_nic() {
### 注：ens6 要替换为dpdk要绑定的网口名
    if /usr/local/bin/dpdk-devbind.py -s | grep drv=igb_uio; then
        echo "已有网口绑定igb_uio"
    else
        echo "查找ens6和eth1口，或者可用的已解绑内核驱动的网口"
        local ens6_pci=$(/usr/local/bin/dpdk-devbind.py -s|egrep 'unused=[^,]*,igb_uio|ens6|eth1'|awk '{print $1}')
        if ip a|grep ens6;then
            echo "找到ens6口,先解绑"
            ifconfig ens6 down
            /usr/local/bin/dpdk-devbind.py -u ens6
        elif ip a|grep eth1;then
            echo "找到eth1口,先解绑"
            ifconfig eth1 down
            /usr/local/bin/dpdk-devbind.py -u eth1
        fi
        if [ -z "$ens6_pci" ];then
            echo "尝试绑定到未绑驱动的网口"
            other_devices=$(/usr/local/bin/dpdk-devbind.py -s | awk '/Other Network devices/{flag=1; next} /^$/{flag=0} flag' | grep -v '^=' | awk '{print $1}')
            if [ ! -z "$other_devices" ]; then
                # 取第一个符合条件的PCI地址
                target_pci=$(echo "$other_devices" | head -n1)
                echo "找到处于'Other Network devices'状态的网口: PCI=$target_pci 尝试绑定"
                /usr/local/bin/dpdk-devbind.py -b igb_uio "$target_pci"
                return
            fi
        else
            echo "尝试将ens6/eth1口绑定到igb_uio"
            /usr/local/bin/dpdk-devbind.py -b igb_uio $ens6_pci
        fi
    fi
}

set_hugepages
insmod_dpdk_io
bind_nic