echo "Sending print command to nvmevirt..."
if [ $1 = "-p" ]; then
    sudo nvme io-passthru /dev/nvme0 --opcode=0xff --namespace-id=1 --cdw2=1
else
    sudo nvme io-passthru /dev/nvme0 --opcode=0xff --namespace-id=1 
fi

if [ $? -eq 0 ]; then
    echo "check log by dmesg"
else
    echo "failed to send command"
fi