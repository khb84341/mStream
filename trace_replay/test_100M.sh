cp 100M.mp4 data/data/media/0/DCIM/Camera/
sync data/data/media/0/DCIM/Camera/100M.mp4

du -sh data/data/* > result/size_cp1
df | grep "temp_tg" > result/df_cp1
cat /sys/kernel/debug/f2fs/status > result/status_cp1
cat /sys/kernel/debug/f2fs/mtype > result/mtype_cp1
cat /sys/devices/virtual/block/temp_tg/pblk/stats > result/pblk_cp1
dmesg > result/dmesg_cp1
date >> result/test_time
