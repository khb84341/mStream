#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a WhiteBox-SSD (OpenChannel-SSD, or OCSSD)
# Support OCSSD versions: Spec 1.2 and 2.0 supported

#-------------------------------------------------------------------------------
# Global parameters you can customize
#-------------------------------------------------------------------------------

# VM image directory
#IMGDIR=/home/yjkang/
# Virtual machine disk image
OSIMGF=/home/yjkang/mstream/ubuntu-jjy.qcow

# OCSSD Spec version (1 for Spec 1.2, and 2 for Spec 2.0)
OCVER=1

# Configurable SSD controller layout parameters (must be power of 2)
#ssd_size=4096             # Emulated SSD size in MegaBytes (MB)
ssd_size=16384             # Emulated SSD size in MegaBytes (MB)
#ssd_size=18432             # Emulated SSD size in MegaBytes (MB)

#num_channels=2            # Number of channels
num_channels=1            # Number of channels
#num_chips_per_channel=4   # Number of NAND flash chips/dies per channel
num_chips_per_channel=2   # Number of NAND flash chips/dies per channel

# End of customizable paramter section
#-------------------------------------------------------------------------------


if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script"
	echo ""
	exit
fi

if [[ $OCVER != 1 && $OCVER != 2 ]]; then
    echo ""
    echo " ==> Unsupported OCSSD Version: $OCVER"
    echo "     Set OCVER to 1 for OCSSD 1.2, and 2 for OCSSD 2.0 support"
    echo ""
    exit
fi


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
# Better not change the following parameters (Do NOT change me)
#
# If you do intend to change these parameters, please help report any bugs you
# meet when using FEMU in OCSSD mode.
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
#num_planes_per_chip=2
num_planes_per_chip=2
#num_pages_per_block=512
#num_pages_per_block=256 # YJ
num_pages_per_block=256
num_sectors_per_page=4
hw_sector_size=4096 # bytes
oob_size=16 # bytes


# Compose the entire FEMU OCSSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",lver=${OCVER}"
FEMU_OPTIONS=${FEMU_OPTIONS}",lmetasize=${oob_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nlbaf=5,lba_index=3,mdts=10"
FEMU_OPTIONS=${FEMU_OPTIONS}",lnum_ch=${num_channels}"
FEMU_OPTIONS=${FEMU_OPTIONS}",lnum_lun=${num_chips_per_channel}"
FEMU_OPTIONS=${FEMU_OPTIONS}",lnum_pln=${num_planes_per_chip}"
FEMU_OPTIONS=${FEMU_OPTIONS}",lsec_size=${hw_sector_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",lsecs_per_pg=${num_sectors_per_page}"
FEMU_OPTIONS=${FEMU_OPTIONS}",lpgs_per_blk=${num_pages_per_block}"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=0"
FEMU_OPTIONS=${FEMU_OPTIONS}",flash_type=1" # 3 for TLC, 4 for QLC, 2 (default) for MLC
#FEMU_OPTIONS=${FEMU_OPTIONS}",cell_type=2" # 3 for TLC, 4 for QLC, 2 (default) for MLC

#echo ${FEMU_OPTIONS}


#-------------------------------------------------------------------------------
# Launch the FEMU VM
sudo x86_64-softmmu/qemu-system-x86_64 \
    -name "FEMU-OCSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 12 \
    -m 8G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::3333-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait
