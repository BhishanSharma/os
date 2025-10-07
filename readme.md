# 🚀 Welcome to the World of Idiots 👋

![header](https://i.imgur.com/3Z8tGzU.png)

Hey there! If you're reading this, congratulations — either you're really into computers, or you just stumbled here wandering through the vast universe of GitHub projects. Either way, you’re welcome! 😎

---

## 💡 What’s the Idea?

Honestly, there’s nothing earth-shattering here. Just another “idiot” trying to write their own operating system.

I’ve always dreamed of building my own OS, and after countless hours exploring the depths of the internet and debugging my own mistakes, I’m finally **close to achieving it**.

> Thanks to the internet, sheer patience, and too much caffeine ☕

![idea](https://i.imgur.com/WZedJxH.png)

---

## 🖥️ What’s in This OS?

This is **version 0.1**, minimalist by design. But hey, even minimalism can be powerful! 💪

This OS is:

* **Terminal-based** — pure text action.
* **Ring 0 / Kernel-space only** — every task runs directly on the CPU.
* **Bare-metal** — no Linux, no Unix layer, just your hardware and the OS.

**Future Goals:** More features, more fancy stuff, and maybe competing with real OSes one day… or at least impressing ourselves.

![kernel](https://i.imgur.com/VVPRTFS.png)

---

## ⚙️ Compatibility

| Component      | Requirement                |
| -------------- | -------------------------- |
| 💾 RAM         | Minimum 1 MB               |
| 🗄️ Storage    | Minimum 8 GB               |
| 🖥️ CPU        | x86_64 (Intel recommended) |
| ⌨️ Peripherals | Keyboard & VGA monitor     |

![compatibility](https://i.imgur.com/7Z0TzQx.png)

---

## ✨ Features

| Feature          | Description              | Icon                                        |
| ---------------- | ------------------------ | ------------------------------------------- |
| FAT32 Filesystem | Read & write files       | ![fat32](https://i.imgur.com/VXxYzZf.png)   |
| Shell Scripting  | Command-line interface   | ![shell](https://i.imgur.com/3POC4Tb.png)   |
| C File Support   | Compile & run C programs | ![c](https://i.imgur.com/E0Vv4aA.png)       |
| Networking       | Basic network stack      | ![network](https://i.imgur.com/XZfD2mY.png) |
| Text Editor      | Minimal editor inside OS | ![editor](https://i.imgur.com/8RmIt3p.png)  |

> Think of it as a tiny but mighty OS lab in your hands.

---

## 🛠️ How to Build Your ISO

### **Stage 1: Clone the Repo**

```bash
git clone https://github.com/BhishanSharma/os.git
```

![clone](https://i.imgur.com/5C8mD4X.png)

---

### **Stage 2: Build Docker Container**

> Only needed once if you’re new to Docker or not changing the build environment.

```bash
docker build buildenv -t myos-buildenv
```

* `buildenv` → folder containing the Dockerfile
* `myos-buildenv` → name of the Docker environment (you can rename it!)

![docker](https://i.imgur.com/mcJcHph.png)

---

### **Stage 3: Build Your ISO**

```bash
docker run --rm -it -v ~/os:/root/env myos-buildenv
make build-x86_64
```

✅ Your freshly baked ISO will appear at:

```
dist/x86_64/kernel.iso
```

![iso](https://i.imgur.com/KGv1VvE.png)

---

### **Stage 4: Create a FAT32 Partition (Optional, for QEMU users)**

```bash
sudo rm -f disk.img
sudo dd if=/dev/zero of=disk.img bs=1M count=32
sudo mkfs.fat -F 32 disk.img
sudo chmod 666 disk.img
mkdir -p /tmp/disk_mount
sudo mount -o loop disk.img /tmp/disk_mount
echo "Hello from FAT32!" | sudo tee /tmp/disk_mount/test.txt
echo "Readme file content" | sudo tee /tmp/disk_mount/readme.txt
sudo umount /tmp/disk_mount
echo "disk.img created successfully!"
```

![fat32\_disk](https://i.imgur.com/jpFsh7J.png)

> Creates a 32 MB FAT32 disk image to test file operations. 🗄️

---

### **Stage 5: Run Your OS in QEMU**

```bash
qemu-system-x86_64 \
    -cdrom dist/x86_64/kernel.iso \
    -hda disk.img \
    -boot d \
    -device rtl8139,netdev=n0 \
    -netdev user,id=n0
```

* Boot your OS in a virtual environment safely 🖥️
* Test networking, shell commands, and more

![qemu](https://i.imgur.com/ykl3JdX.gif)

---

## 🎨 Footer

Made with ☕ + 💻 + 🧠 by an **idiot**.

![footer](https://i.imgur.com/8F9kJqY.png)
