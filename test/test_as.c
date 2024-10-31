#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../spin_emu.h"


int main() {
    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    const char *server_ip = "127.0.0.1";

    // create sdp message
    sdp_msg_t msg;

    memset(&msg, 0, sizeof(msg));

    msg.next = NULL; 
    msg.checksum = 0xff;

    msg.length = sizeof(msg);

    msg.flags = 0x07;
    msg.tag = 0;

    msg.dest_port = 0;
    msg.srce_port = 0;

    // send to core x = 1, y = 1
    msg.dest_addr = (0x01 << 4) | 0x01; 
    msg.srce_addr = 0;

    msg.cmd_rc = CMD_AS;
    msg.seq = 0;
    

    strncpy((char *)msg.data, "sending data to emu", sizeof(msg.data) - 1);
    
    
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
    
    int numbytes = sendto(sockfd, &msg, sizeof(msg), 0, p->ai_addr, p->ai_addrlen);
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
