# Daemonless HP M1136 Print Pipeline on Unrooted Android

This document outlines a custom hardware-software pipeline to print to a host-based USB printer (HP LaserJet M1136) from an unrooted Android device. It bypasses SELinux restrictions using a C-based kernel `ioctl` bridge and processes proprietary `glibc` binaries inside an Ubuntu PRoot container.

## Architecture Overview
* **The Translation Engine (Ubuntu PRoot):** Provides the `glibc` environment and Ghostscript engines required to run the closed-source HP proprietary raster-to-binary filter (`hpcups`).
* **The Hardware Bridge (Termux Native):** Bypasses the Android SELinux sandbox using a custom C program to wrap the USB file descriptor and perform raw kernel `ioctl` bulk transfers.
* **The Connection:** Localhost TCP port `9100`.

---

## Phase 1: The Hardware Bridge (Termux Native)

update
```
pkg update -y && yes | pkg upgrade
```

install dependencies
```
pkg i termux-api clang
```

Construct the pure C socket-to-USB listener to bypass Android's restricted `/sys/` hardware tree. 

**1. Generate the C Source Code:**
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

**2. Compile the Kernel Bridge:**
```bash
clang -o bridge bridge.c
```

---

## Phase 2: The Translation Engine (Ubuntu PRoot)

Install and isolate the standard Linux environment, PDF rendering engines, and proprietary HP binaries.

**1. Install Ubuntu PRoot via Termux:**
```bash
pkg install proot-distro
proot-distro install ubuntu
proot-distro login ubuntu
```

**2. Install Core Dependencies & PDF Renderers:**
```bash
apt update
apt install hplip cups nano wget ghostscript poppler-utils cups-filters -y
```

**3. Install Proprietary HP Plugin:**
```bash
hp-plugin -i
```
*(Accept the license and install the proprietary blob).*

**4. Generate the Hardware PPD File:**
The CUPS daemon must be temporarily started to extract the specific `.ppd` file for the M1136.
```bash
/usr/sbin/cupsd
PPD_URI=$(lpinfo -m | grep -i m1136 | awk '{print $1}' | head -n 1)
lpadmin -p HP_M1136 -E -v file:///dev/null -m "$PPD_URI"
```
Verify the file exists and is populated (~11KB):
```bash
ls -lh /etc/cups/ppd/HP_M1136.ppd
```
Exit the PRoot environment:
```bash
exit
```

---

## Phase 3: The Execution Workflow

To print a document (e.g., a PDF), execute the following steps.

**1. Copy Document to Shared Directory (Termux Native):**
Move your target file into the Termux `tmp` directory so Ubuntu can read it.
```bash
cp ~/downloads/your_document.pdf $PREFIX/tmp/doc.pdf
```

**2. Start the Hardware Bridge (Termux Native):**
Find the exact USB bus path and execute the bridge. Keep this terminal session open.
```bash
termux-usb -l
# Output example: "/dev/bus/usb/001/003"
termux-usb -r -e ./bridge /dev/bus/usb/001/003
```

**3. Translate and Blast (Ubuntu PRoot):**
Open a second Termux session. Log into Ubuntu using the `--shared-tmp` flag, set the layout options to prevent stretching, and push the file through the daemonless pipeline.
```bash
proot-distro login ubuntu --shared-tmp
```
```bash
# Define layout constraints
OPTIONS="PageSize=A4 fit-to-page Resolution=600x600dpi"

# Step A: Convert PDF/Text to CUPS Raster
cupsfilter -P /etc/cups/ppd/HP_M1136.ppd -o "$OPTIONS" -m application/vnd.cups-raster /tmp/doc.pdf > /tmp/job.raster

# Step B: Translate Raster to HP Proprietary Binary
PPD=/etc/cups/ppd/HP_M1136.ppd CUPS_DATADIR=/usr/share/cups /usr/lib/cups/filter/hpcups 1 root "TermuxPrint" 1 "$OPTIONS" /tmp/job.raster > /tmp/final.bin

# Step C: Blast to Hardware Bridge
nc 127.0.0.1 9100 < /tmp/final.bin

# Cleanup
rm -f /tmp/job.raster /tmp/final.bin
```
