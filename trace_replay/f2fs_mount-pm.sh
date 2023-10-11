rmmod f2fs
insmod mtftl/fs/f2fs/f2fs.ko
umount data
./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 5
./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 6
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 3

#mkfs.f2fs
#mkfs.f2fs -s 8 /dev/temp_tg -f -o "nobarrier"

mount -t f2fs /dev/temp_tg data -o "discard"
mount | grep "f2fs"
