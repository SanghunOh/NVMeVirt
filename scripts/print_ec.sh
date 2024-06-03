echo "Sending print command to nvmevirt..."
if [ -n "$1" ] && [ "$1" = "-p" ]; then
    sudo nvme io-passthru /dev/nvme1 --opcode=0xfe --namespace-id=1 --cdw2=1 > /dev/null 2>&1
elif [ "$1" = "-pe" ]; then
    sudo nvme io-passthru /dev/nvme1 --opcode=0xfe --namespace-id=1 --cdw2=2 > /dev/null 2>&1
else
    sudo nvme io-passthru /dev/nvme1 --opcode=0xfe --namespace-id=1 > /dev/null 2>&1
fi

if [ $? -eq 0 ]; then
    echo "check log by dmesg"
else
    echo "failed to send command"
fi