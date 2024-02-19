dev=/dev/nvme0n1
mount=~/mount

echo "insert nvmevirt module..."
sudo insmod ~/nvmev.ko memmap_start=16G memmap_size=16G cpus=7,8

if [ $? -eq 0 ]; then
        echo "make file system on $dev"
        sudo mkfs -t ext4 $dev
        if [ $? -eq 0 ]; then
                echo "mount on $mount directory"
                sudo mount $dev $mount
        else
                echo "mount failed"
        fi
else
        echo "mkfs failed"