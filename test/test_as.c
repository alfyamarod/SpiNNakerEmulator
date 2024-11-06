#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stddef.h>

#include "../spin_emu.h"



uint8_t pack_cpu_port(uint8_t cpu, uint8_t port) {
    // Ensure CPU and Port are within their respective bit ranges
    if (cpu > 31 || port > 7) {
        fprintf(stderr, "Invalid CPU (%u) or Port (%u) number\n", cpu, port);
        exit(EXIT_FAILURE);
    }
    return (port << 5) | (cpu & 0x1F);
}


int main() {
    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    const char *server_ip = "127.0.0.1";

    // create sdp message
    sdp_msg_t msg;


    memset(&msg, 0, sizeof(msg));

    msg.next = (void *)0;

    msg.length = htons(sizeof(sdp_msg_t));

    
    msg.checksum = htons(0xFFFF);

    msg.flags = 0x07;
    msg.tag = 0;

    msg.dest_port = pack_cpu_port(1, 0);
    msg.srce_port = pack_cpu_port(31, 7);

    msg.dest_addr = htons((1 << 8) | 1);
    msg.srce_addr = htons((1 << 8) | 1);

    msg.cmd_rc = htons(10);

    msg.seq = htons(0);
    msg.arg1 = htonl(0);    
    msg.arg2 = htonl(0);    
    msg.arg3 = htonl(0);    


    strncpy((char *)msg.data, "Hello, SpiNNaker!", sizeof(msg.data) - 1);  // User data

    
    // Prepare the hints structure
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;         // Use IPv4
    hints.ai_socktype = SOCK_DGRAM;    // Use UDP

    // Convert port number to a string
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", SPIN_EMU_PORT);

    // Get server info
    if ((rv = getaddrinfo(server_ip, port_str, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // Loop through all the results and create a socket
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("socket");
            continue;
        }
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "failed to create socket\n");
        return 2;
    }

    // Send the message to the server
    size_t send_offset = offsetof(sdp_msg_t, flags);
    printf("send offset %d \n", send_offset);
    size_t send_size = 2 + sizeof(sdp_msg_t) - send_offset;
    uint8_t *send_buff = malloc(send_size);

    // 2 byte padding required to embedd in UDP
    send_buff[0] = 0;
    send_buff[1] = 0;

    memcpy(send_buff + 2, ((uint8_t *)&msg) + send_offset, sizeof(sdp_msg_t) - send_offset);

    
    int numbytes = sendto(sockfd, send_buff, send_size, 0, p->ai_addr, p->ai_addrlen);
    if (numbytes == -1) {
        perror("sendto");
        close(sockfd);
        freeaddrinfo(servinfo);
        return 3;
    }

    printf("Sent %d bytes to %s:%d\n", numbytes, server_ip, SPIN_EMU_PORT);


    // Clean up
    close(sockfd);
    freeaddrinfo(servinfo);
    return 0;
}
