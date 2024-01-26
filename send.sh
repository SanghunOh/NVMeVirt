PORT=12023
FILE=$(pwd)'/'$1

if [ -n "$1" ]; then
    scp -P $PORT -i ~/.ssh/nctr241.pem $FILE nv@localhost:~/
fi
