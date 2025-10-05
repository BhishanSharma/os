# To build a docker env
docker build buildenv -t myos-buildenv
## buildenv is the folder containing the Dockerfile
## myos-buildenv is the name of the docker env

# To run this docker env
docker run --rm -it -v ~/os:/root/env myos-buildenv
## ~/os - folder in which you wnat to run the env
## /root/env - workdir you have saved in docker
## myos-buildenv - name of the docker env

# After entering the env build the project using makefile
make build-x86_64
## build-x86_64 - name of the function in main file from where to start build

# Creating a partition in fat32 for the os to work
sudo rm -f disk.img && sudo dd if=/dev/zero of=disk.img bs=1M count=32 && sudo mkfs.fat -F 32 disk.img && sudo chmod 666 disk.img && mkdir -p /tmp/disk_mount && sudo mount -o loop disk.img /tmp/disk_mount && echo "Hello from FAT32!" | sudo tee /tmp/disk_mount/test.txt && echo "Readme file content" | sudo tee /tmp/disk_mount/readme.txt && sudo umount /tmp/disk_mount && echo "disk.img created successfully!"

# To run the iso using qemu
qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -hda disk.img -boot d