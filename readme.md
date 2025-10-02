# To build a docker env
docker build buildenv -t myos-buildenv
## buildenv is the folder containing the Dockerfile
## myos-buildenv is the name of the docker env

# To run this docker env
docker run --rm -it -v G:\others\Dream\os\new:/root/env myos-buildenv
## G:\others\Dream\os\new - folder i which you wnat to run the env
## /root/env - workdir you have saved in docker
## myos-buildenv - name of the docker env

# After entering the env build the project using makefile
make build-x86_64
## build-x86_64 - name of the function in main file from where to start build

# To run the iso using qemu
qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso