modprobe pblk
nvme lnvm create -d nvme0n1 -t pblk -n temp_tg -b 0 -e 1 -f

grep "NR_CURSEG_DATA" YJ/f2fs-tools/f2fs-tools/include/f2fs_fs.h | grep -v "//" | grep -v "NODE"
grep "cp source/" YJ/run_replay.sh | grep -v "#"
grep "F2FS_FGROUP" YJ/mtftl/fs/f2fs/f2fs.h | grep "#define"

