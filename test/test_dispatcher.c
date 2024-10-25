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
    const char *message = "Hello, server!";
    
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
    int numbytes = sendto(sockfd, message, strlen(message), 0, p->ai_addr, p->ai_addrlen);
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
