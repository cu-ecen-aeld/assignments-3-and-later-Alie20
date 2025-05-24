#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-
ORIG_DIR=$(pwd)   # Save current directory


if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    #Configure the kernel
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    #Build the kernel
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
fi

cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image "${OUTDIR}/"
echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr,var}
mkdir -p rootfs/usr/{bin,lib,sbin}
mkdir -p rootfs/var/log



cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
#git clone git://busybox.net/busybox.git
git clone https://git.busybox.net/busybox
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    
    # TODO:  Configure busybox
    #configure the busybox
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    #make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} menuconfig

else
    cd busybox
fi

# TODO: Make and install busybox
	#build the busybox
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
# Get SYSROOT from your toolchain
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
cp -v ${SYSROOT}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib/

# Copy shared libraries
cp -v ${SYSROOT}/lib64/libm.so.6        ${OUTDIR}/rootfs/lib64/
cp -v ${SYSROOT}/lib64/libresolv.so.2   ${OUTDIR}/rootfs/lib64/
cp -v ${SYSROOT}/lib64/libc.so.6        ${OUTDIR}/rootfs/lib64/


# TODO: Make device nodes
cd ${OUTDIR}/rootfs/dev

# Create /dev/null (character device, major 1, minor 3)
sudo mknod -m 666 null c 1 3

# Create /dev/console (character device, major 5, minor 1)
sudo mknod -m 600 console c 5 1

# TODO: Clean and build the writer utility
cd "$ORIG_DIR"    # Go back to where the script was run from
 
${CROSS_COMPILE}gcc -Wall -Werror writer.c -o ${OUTDIR}/rootfs/home/writer

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
cd "$ORIG_DIR"
cp autorun-qemu.sh ${OUTDIR}/rootfs/home/
cp finder-test.sh ${OUTDIR}/rootfs/home/
cp -r conf ${OUTDIR}/rootfs/home/
cp finder.sh ${OUTDIR}/rootfs/home/
cp conf/assignment.txt ${OUTDIR}/rootfs/home

sed -i 's|../conf/assignment.txt|/home/assignment.txt|' ${OUTDIR}/rootfs/home/finder-test.sh

# TODO: Chown the root directory
sudo chown root:root ${OUTDIR}/rootfs
# TODO: Create initramfs.cpio.gz
cd "$OUTDIR/rootfs"
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio




