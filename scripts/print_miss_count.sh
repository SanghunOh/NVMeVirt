echo "Sending print command to nvmevirt..."
sudo nvme io-passthru /dev/nvme0 --opcode=0xff --namespace-id=1 > /dev/null 2>&1
# fi

if [ $? -eq 0 ]; then
    echo "check log by dmesg"
else
    echo "failed to send command"
fi