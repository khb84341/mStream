#cp source/conf-UA.json conf.json
#cp source/conf-UB.json conf.json
#cp source/conf-UC.json conf.json
#cp source/conf-test.json conf.json
#cp source/conf-UD.json conf.json

mkdir result
rm replay*.out
rm result/*
rm data/* -r
date > result/test_time
./traceReplay -v -i -l -d $1 conf.json
#./traceReplay -i -l -d $1 conf.json
du -sh data/data/* > result/size_$1
df | grep "temp_tg" > result/df_$1
cat /sys/kernel/debug/f2fs/status > result/status_$1
cat /sys/kernel/debug/f2fs/mtype > result/mtype_$1
cat /sys/devices/virtual/block/temp_tg/pblk/stats > result/pblk_$1
dmesg > result/dmesg_$1
date >> result/test_time

cp mtftl/fs/f2fs/f2fs.h result/
cp mtftl/fs/ext4/ext4.h result/
cp mtftl/drivers/lightnvm/pblk.h result/

#./test_100M.sh
#./test_cp.sh
