echo "Sending print command to nvmevirt..."
if [ -n "$1" ] && [ "$1" = "-p" ]; then
    sudo nvme io-passthru /dev/nvme0 --opcode=0xff --namespace-id=1 --cdw2=1 > /dev/null 2>&1
else
    sudo nvme io-passthru /dev/nvme0 --opcode=0xff --namespace-id=1 > /dev/null 2>&1
fi

if [ $? -eq 0 ]; then
    echo "check log by dmesg"
else
    echo "failed to send command"
fi