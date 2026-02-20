# Daemonless HP M1136 Print Pipeline on Unrooted Android

## Architecture Overview
* **The Translation Engine (Ubuntu PRoot):** Provides the `glibc` environment required to run the closed-source HP proprietary raster-to-binary filter (`hpcups`).
* **The Hardware Bridge (Termux Native):** Bypasses the Android SELinux sandbox using a custom C program to wrap the USB file descriptor and perform raw kernel `ioctl` bulk transfers.
* **The Connection:** Localhost TCP port `9100`.

---

## Phase 1: The Translation Engine (Ubuntu PRoot)

Install and isolate the standard Linux environment to handle the proprietary HP binaries.

**1. Install Ubuntu PRoot via Termux:**
```bash
pkg install proot-distro
proot-distro install ubuntu
proot-distro login ubuntu
```

**2. Install Core Dependencies & Proprietary HP Plugin:**
```bash
apt update
apt install hplip cups nano wget -y
hp-plugin -i
```

**3. Verify Printer Profile and Daemon Initial Execution:**
```bash
lpinfo -m | grep -i m1136
/usr/sbin/cupsd
lpstat -p HP_M1136
```
*(Note: At this stage, Android kernel UID leaking causes standard `cupsd` spooling to crash, which is verified by checking `tail -n 20 /var/log/cups/error_log`. This necessitates the daemonless pipeline below).*

---

## Phase 2: The Hardware Bridge (Termux C Native)

Construct the pure C socket-to-USB listener to bypass Android's restricted `/sys/` hardware tree. 

**1. Generate the C Source Code using a Heredoc:**
```c
cat << 'EOF' > bridge.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/usbdevice_fs.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Error: Missing FD\n");
        return 1;
    }
    int fd = atoi(argv[1]);

    int interface = 0;
    ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(9100) };
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 3);
    printf("[*] Kernel ioctl Bridge Listening on 127.0.0.1:9100\n");

    int conn = accept(server_fd, NULL, NULL);
    printf("[*] Receiving payload... blasting directly via Kernel ioctl!\n");

    unsigned char buf[8192];
    int valread;
    struct usbdevfs_bulktransfer bulk;
    bulk.timeout = 5000;
    bulk.data = buf;

    while ((valread = read(conn, buf, sizeof(buf))) > 0) {
        bulk.len = valread;
        int success = 0;
        
        for (int ep = 1; ep <= 3; ep++) {
            bulk.ep = ep;
            if (ioctl(fd, USBDEVFS_BULK, &bulk) >= 0) {
                success = 1;
                break;
            }
        }
        
        if (!success) {
            printf("[!] Kernel rejected bulk transfer!\n");
        }
    }

    printf("[*] Payload delivered to hardware!\n");
    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &interface);
    close(conn);
    close(server_fd);
    return 0;
}
EOF
```

**2. Compile and Launch the Kernel Bridge:**
```bash
clang -o bridge bridge.c
termux-usb -r -e ./bridge /dev/bus/usb/001/003
```
*(The bridge will hold port `9100` open and listen for the proprietary payload).*

---

## Phase 3: The Execution (Daemonless Pipeline)

With the C bridge actively listening in Termux, execute the translation pipeline manually inside the Ubuntu PRoot.

**1. Translate Text/PDF to CUPS Raster:**
```bash
cupsfilter -P /etc/cups/ppd/HP_M1136.ppd -m application/vnd.cups-raster test.txt > job.raster
```

**2. Force Raster through Closed-Source `hpcups` Plugin:**
```bash
PPD=/etc/cups/ppd/HP_M1136.ppd CUPS_DATADIR=/usr/share/cups /usr/lib/cups/filter/hpcups 1 root "Test" 1 "" job.raster > final_payload.bin
```

**3. Verify Output Payload:**
```bash
ls -lh final_payload.bin
```
*(A non-zero file size confirms the proprietary binary blob generated successfully).*

**4. Fire Payload into the Hardware Bridge:**
```bash
nc 127.0.0.1 9100 < final_payload.bin
```
*(The data immediately pushes through the localhost port, hits the C script, and transfers directly to the printer's RAM).*
