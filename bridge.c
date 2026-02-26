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
