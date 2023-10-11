umount data

# FTL OP=5
mkfs.ext4 -G 16 -E discard /dev/temp_tg
mount -t ext4 -o noatime,noauto_da_alloc,nosuid,nodev,mstream,discard,stripe=4096 /dev/temp_tg data

# OP=10
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 16

# fTL OP=3, OP=2
#./f2fs-tools/f2fs-tools/mkfs/mkfs.f2fs -s 8 /dev/temp_tg -f -o 22

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

#mount -t f2fs /dev/temp_tg data
