cp source/conf-A.json conf.json
#cp source/conf-B.json conf.json

mkdir result
rm replay*.out
cp result/* result-backup/
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
cp mtftl/fs/f2fs/gc.c result/
cp mtftl/fs/f2fs/stream.c result/f2fs_stream.c
cp mtftl/fs/f2fs/kmeans.c result/f2fs_kmeans.c
cp mtftl/fs/ext4/ext4.h result/
cp mtftl/drivers/lightnvm/pblk.h result/

#./test_cp.sh
