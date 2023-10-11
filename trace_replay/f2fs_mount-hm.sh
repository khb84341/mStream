rmmod f2fs
insmod mtftl/fs/f2fs/f2fs.ko
umount data
./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o "nobarrier"

# FTL OP=5
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 20

# OP=10
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 16

# fTL OP=3, OP=2
./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 22

# fTL OP=4
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 21

# fTL OP=7
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 18

# FTL OP=8
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 17

# FTL OP=15
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 11

# fTL OP=6
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 19

# fTL OP=9
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 16

# fTL OP=2
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 22


#mkfs.f2fs
#mkfs.f2fs -s 8 /dev/temp_tg -f -o "nobarrier"

mount -t f2fs /dev/temp_tg data -o "discard"
mount | grep "f2fs"
