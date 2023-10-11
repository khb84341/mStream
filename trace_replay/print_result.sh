logday=$1

du -sh data/data/* > result/size_${logday}
cat /sys/kernel/debug/f2fs/status > result/status_${logday}
cat /sys/kernel/debug/f2fs/mtype > result/mtype_${logday}
cat /sys/fs/ext4/temp_tg/user_writes > result/ext4_datawrites_${logday}
cat /sys/devices/virtual/block/temp_tg/pblk/stats > result/pblk_${logday}
dmesg > result/dmesg_${logday}
echo ${logday} >> result/curtime
date >> result/curtime
df | grep "temp_tg" > result/df_${logday}
cp conf.json result/conf.json

#test_str="${logday%%.*}"
#echo ${test_str}

if [ "${logday}" == "60" ];
then
	echo > /sys/kernel/debug/tracing/trace
	echo 4 > /sys/kernel/debug/tracing/buffer_size_kb
	echo 4096 > /sys/kernel/debug/tracing/buffer_size_kb
	echo 1 > /sys/kernel/debug/tracing/tracing_on
	cat /sys/kernel/debug/tracing/tracing_on
fi

