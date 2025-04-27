/*
* netsio.c - NetSIO interface for FujiNet-PC <-> Atari800 Emulator
*
* Uses two threads:
*  - fujinet_rx_thread: receive from FujiNet-PC, respond to pings/alives, queue complete packets to emulator
*  - emu_tx_thread: receive from emulator FIFO, queue complete packets to FujiNet-PC
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>  /* For inet_ntoa() */
#include <fcntl.h>      /* For fcntl() */
#include "netsio.h"
#include "log.h"
#include "pia.h" /* For toggling PROC & INT */

/* Flag to know when netsio is enabled */
volatile int netsio_enabled = 0;
/* Holds sync to fujinet-pc incremented number */
uint8_t netsio_sync_num = 0;
/* if we have heard from fujinet-pc or not */
int fujinet_known = 0;
/* wait for fujinet sync if true */
int netsio_sync_wait = 0;
/* true if cmd line pulled */
int netsio_cmd_state = 0;

/* FIFO pipes:
* fds0: FujiNet->emulator
* fds1: emulator->FujiNet
*/
int fds0[2], fds1[2];

/* UDP socket for NetSIO and return address holder */
#define FUJINET_PORT 65408  /* Default FujiNet port */

static int sockfd = -1;
static struct sockaddr_storage fujinet_addr;
static socklen_t fujinet_addr_len = sizeof(fujinet_addr);
static int connection_established = 0;  /* Flag to track if we've established a connection */

#ifdef FUJINET_DEBUG
static char fujinet_ip[INET_ADDRSTRLEN]; /* To hold IP string for debugging */
#endif

/* Thread declarations */
static void *fujinet_rx_thread(void *arg);
static void *emu_tx_thread(void *arg);
static void reset_socket(void);

char *buf_to_hex(const uint8_t *buf, size_t offset, size_t len) {
    /* each byte takes "XX " == 3 chars, +1 for trailing NUL */
    size_t needed = len * 3 + 1;
    char *s = malloc(needed);
    size_t i = 0;
    if (!s) return NULL;
    char *p = s;
    for (i = 0; i < len; i++) {
        sprintf(p, "%02X ", buf[offset + i]);
        p += 3;
    }
    if (len) {
        p[-1] = '\0';
    } else {
        *p = '\0';
    }
    return s;
}

/* write data to emulator FIFO (fujinet_rx_thread) */
static void enqueue_to_emulator(const uint8_t *pkt, size_t len) {
    Log_print("netsio: enqueue_to_emulator, len=%zu, data=%s", len, buf_to_hex(pkt, 0, len));
    ssize_t n;
    while (len > 0) {
        n = write(fds0[1], pkt, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("netsio: write to emulator FIFO");
            /*exit(1);*/
        }
        pkt += n;
        len -= n;
    }
}

/* send a packet to FujiNet socket */
static void send_to_fujinet(const uint8_t *pkt, size_t len) {
    Log_print("netsio: send_to_fujinet, len=%zu, data=%s", len, buf_to_hex(pkt, 0, len));
    static int error_count = 0;
    static int total_sends = 0;
    ssize_t n;
    
    if (sockfd < 0) {
        Log_print("netsio: [ERROR] sockfd < 0, cannot send");
        return;
    }

    if (!fujinet_known || fujinet_addr_len == 0) {
        Log_print("netsio: [ERROR] FujiNet address not known, cannot send packet");
        return;
    }

    total_sends++;
    
    /* Use a connected socket if we've established a connection, otherwise fallback to sendto */
    if (connection_established) {
        n = send(sockfd, pkt, len, 0);
    } else {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&fujinet_addr;
        /* Make sure we have a port set - use default if not */
        if (addr_in->sin_port == 0) {
            addr_in->sin_port = htons(FUJINET_PORT);
            Log_print("netsio: Fixed zero port in socket address to default %d", FUJINET_PORT);
        }
        
        Log_print("netsio: Sending packet to %s:%d, len=%zu", 
                 inet_ntoa(addr_in->sin_addr), ntohs(addr_in->sin_port), len);
                 
        n = sendto(sockfd, pkt, len, 0, (struct sockaddr *)&fujinet_addr, fujinet_addr_len);
    }
    
    if (n < 0) {
        error_count++;
        Log_print("netsio: Error sending to FujiNet: %s (errors: %d/%d)", strerror(errno), error_count, total_sends);
        
        /* After multiple consecutive errors, try to reset the socket */
        if (error_count > 5) {
            Log_print("netsio: Too many send errors, resetting socket");
            reset_socket();
            error_count = 0;
        }
    } else {
        /* Successful send, reset error counter */
        error_count = 0;
        
        /* If we're sending to a consistent address and we haven't established a connection yet,
           try to establish one to improve reliability */
        if (!connection_established && total_sends > 10) {
            Log_print("netsio: Establishing persistent connection to FujiNet");
            
            /* Set socket to non-blocking first */
            int flags = fcntl(sockfd, F_GETFL, 0);
            fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
            
            /* Try to connect */
            if (connect(sockfd, (struct sockaddr *)&fujinet_addr, fujinet_addr_len) < 0) {
                if (errno != EINPROGRESS) {
                    Log_print("netsio: Failed to establish connection: %s", strerror(errno));
                } else {
                    Log_print("netsio: Connection in progress");
                    connection_established = 1;
                }
            } else {
                Log_print("netsio: Connection established successfully");
                connection_established = 1;
            }
            
            /* Set socket back to blocking */
            fcntl(sockfd, F_SETFL, flags);
        }
    }
}

/* Reset socket and communication state */
static void reset_socket(void) {
    if (sockfd >= 0) {
        close(sockfd);
    }
    
    /* Create socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        Log_print("netsio: Failed to create socket: %s", strerror(errno));
        return;
    }
    
    /* Clear connection state */
    connection_established = 0;
    netsio_sync_wait = 0;  /* Clear any pending sync wait */
    
    /* Set a relatively short timeout for socket operations */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;  /* 500ms */
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        Log_print("netsio: Failed to set receive timeout: %s", strerror(errno));
    }
    
    Log_print("netsio: Socket reset complete");
}

/* Send a single byte as a DATA_BYTE packet */
void send_byte_to_fujinet(uint8_t data_byte) {
    uint8_t packet[2];
    packet[0] = NETSIO_DATA_BYTE;
    packet[1] = data_byte;
    send_to_fujinet(packet, sizeof(packet));
}

/* Send up to 512 bytes as a DATA_BLOCK packet */
void send_block_to_fujinet(const uint8_t *block, size_t len) {
    if (len == 0 || len > 512) return;  /* sanity check */

    uint8_t packet[512 + 2];
    packet[0] = NETSIO_DATA_BLOCK;
    memcpy(&packet[1], block, len);
    /* Pad the end with a junk byte or FN-PC won't accept the packet */
    packet[1 + len] = 0xFF;
    send_to_fujinet(packet, len + 2);
}

/* Initialize NetSIO:
*   - connect to FujiNet socket
*   - create FIFOs
*   - spawn the two threads
*/
int netsio_init(uint16_t port) {
    struct sockaddr_in addr;
    pthread_t rx_thread, tx_thread;

    /* create emulator <-> netsio FIFOs */
    if (pipe(fds0) < 0 || pipe(fds1) < 0) {
        perror("netsio: pipe");
        return -1;
    }
    /* fds0[0] = emulator reads here (FujiNet->emu)
    fds0[1] = netsio_rx_thread writes here */
    /* fds1[0] = netsio_tx_thread reads here
    fds1[1] = emulator writes here (emu->FujiNet) */

    /* connect socket to FujiNet */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("netsio: socket");
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    /*
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("netsio: connect");
        close(sockfd);
        return -1;
    }*/

    /* Bind to the socket on requested port */
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("netsio bind");
    close(sockfd);
    }

    /* spawn receiver thread */
    if (pthread_create(&rx_thread, NULL, fujinet_rx_thread, NULL) != 0) {
        perror("netsio: pthread_create rx");
        return -1;
    }
    pthread_detach(rx_thread);

    /* spawn transmitter thread */
/* Disabled this thread
    if (pthread_create(&tx_thread, NULL, emu_tx_thread, NULL) != 0) {
        perror("netsio: pthread_create tx");
        return -1;
    }
    pthread_detach(tx_thread);
*/
    return 0;
}

/* Return number of bytes waiting from FujiNet to emulator */
int netsio_available(void) {
    int avail = 0;
    if (fds0[0] >= 0) {
        if (ioctl(fds0[0], FIONREAD, &avail) < 0) {
                Log_print("netsio_avail: ioctl error");
                return -1;
        }
    }
    return avail;
}

/* COMMAND ON */
int netsio_cmd_on(void)
{
    Log_print("netsio: CMD ON");
    netsio_cmd_state = 1;
    uint8_t p = NETSIO_COMMAND_ON;
    send_to_fujinet(&p, 1);
    return 0;
}

/* COMMAND OFF */
int netsio_cmd_off(void)
{
    Log_print("netsio: CMD OFF");
    netsio_cmd_state = 0;
    uint8_t p = NETSIO_COMMAND_OFF;
    send_to_fujinet(&p, 1);
    return 0;
}

/* COMMAND OFF with SYNC */
int netsio_cmd_off_sync(void)
{
    Log_print("netsio: CMD OFF SYNC -- sending CMD OFF SYNC packet and entering sync wait state");
    uint8_t p[2] = { NETSIO_COMMAND_OFF_SYNC, netsio_sync_num };
    Log_print("netsio: CMD OFF SYNC packet sent: [%02X %02X] (sync_num=%u)", p[0], p[1], netsio_sync_num);
    send_to_fujinet(&p, sizeof(p));
    netsio_sync_num++;
    netsio_sync_wait = 1;
    Log_print("netsio: [STATE] netsio_sync_wait SET to 1, now waiting for SYNC RESPONSE (0x81) from FujiNet");
    return 0;
}

/* Toggle Command Line */
void netsio_toggle_cmd(int v)
{
    if (!v)
        netsio_cmd_off_sync();
    else
        netsio_cmd_on();
}

/* The emulator calls this to send a data byte out to FujiNet */
int netsio_send_byte(uint8_t b) {
    Log_print("netsio: netsio_send_byte called, b=%02X", b);
    uint8_t pkt[2] = { NETSIO_DATA_BYTE, b };
    Log_print("netsio: send byte: %02X", b);
    send_to_fujinet(&pkt, 2);
    return 0;
}

/* The emulator calls this to send a data block out to FujiNet */
int netsio_send_block(const uint8_t *block, ssize_t len) {
    Log_print("netsio: netsio_send_block called, len=%zd, data=%s", len, buf_to_hex(block, 0, len));
    /* ssize_t len = sizeof(block);*/ 
    send_block_to_fujinet(block, len);
    Log_print("netsio: send block, %i bytes:\n  %s", len, buf_to_hex(block, 0, len));
}

/* The emulator calls this to receive a data byte from FujiNet */
int netsio_recv_byte(uint8_t *b) {
    int ret;
    ret = read(fds0[0], b, 1);
    /* Log_print("netsio: netsio_recv_byte called, ret=%d, b=%02X", ret, (ret > 0) ? *b : 0);
    if (ret < 0) {
        if (errno == EINTR) return netsio_recv_byte(b);
        perror("netsio: read from rx FIFO");
        return -1;
    }
    if (ret == 0) {
        /* FIFO closed? */
        return -1;
    }
    /* Log_print("netsio: read to emu: %02X", (unsigned)*b);
    return 0;
}

/* Send a test command frame to fujinet-pc */
void netsio_test_cmd(void)
{
    uint8_t p[6] = { 0x70, 0xE8, 0x00, 0x00, 0x59 }; /* Send fujidev get adapter config request */
    netsio_cmd_on(); /* Turn on CMD */
    send_block_to_fujinet(p, sizeof(p));
    /* send_byte_to_fujinet(0x70);
    send_byte_to_fujinet(0xE8);
    send_byte_to_fujinet(0x00);
    send_byte_to_fujinet(0x00);
    send_byte_to_fujinet(0x59); */
    netsio_cmd_off_sync(); /* Turn off CMD */
}

/* Helper function to identify packet types */
static const char* get_packet_type_name(uint8_t cmd) {
    switch(cmd) {
        case 0x81: return "SYNC_ACK"; // FujiNet SYNC RESPONSE
        case 0xC2: return "PING";
        case 0xC3: return "PONG";
        case 0xC0: return "DISCONNECT";
        case NETSIO_DATA_BYTE: return "DATA_BYTE";
        case NETSIO_DATA_BLOCK: return "DATA_BLOCK";
        case NETSIO_DATA_BYTE_SYNC: return "DATA_BYTE_SYNC";
        case NETSIO_COMMAND_OFF: return "COMMAND_OFF";
        case NETSIO_COMMAND_ON: return "COMMAND_ON";
        case NETSIO_COMMAND_OFF_SYNC: return "COMMAND_OFF_SYNC";
        case NETSIO_MOTOR_OFF: return "MOTOR_OFF";
        case NETSIO_MOTOR_ON: return "MOTOR_ON";
        case NETSIO_PROCEED_OFF: return "PROCEED_OFF";
        case NETSIO_PROCEED_ON: return "PROCEED_ON";
        case NETSIO_INTERRUPT_OFF: return "INTERRUPT_OFF";
        case NETSIO_INTERRUPT_ON: return "INTERRUPT_ON";
        case NETSIO_SPEED_CHANGE: return "SPEED_CHANGE";
        case NETSIO_ALIVE_REQUEST: return "ALIVE_REQUEST";
        case NETSIO_ALIVE_RESPONSE: return "ALIVE_RESPONSE";
        case NETSIO_CREDIT_STATUS: return "CREDIT_STATUS";
        case NETSIO_CREDIT_UPDATE: return "CREDIT_UPDATE (SYNC_RESPONSE)";
        case NETSIO_WARM_RESET: return "WARM_RESET";
        case NETSIO_COLD_RESET: return "COLD_RESET";
        default: return "UNKNOWN";
    }
}

/* Thread: receive from FujiNet socket (one packet == one command) */
static void *fujinet_rx_thread(void *arg) {
    uint8_t buf[65536];

    while (1) {
        struct sockaddr_storage src_addr;
        socklen_t src_addr_len = sizeof(src_addr);
        
        ssize_t n = recvfrom(sockfd,
                             buf,
                             sizeof(buf),
                             0,
                             (struct sockaddr*)&src_addr,
                             &src_addr_len);
                             
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                Log_print("netsio: recvfrom error: %s", strerror(errno));
            }
            continue;
        }
        
        if (n > 0) {
            uint8_t cmd = buf[0];
            const char* cmd_name = get_packet_type_name(cmd);

            if (!fujinet_known) {
                /* Remember the source address for replies */
                memcpy(&fujinet_addr, &src_addr, src_addr_len);
                fujinet_addr_len = src_addr_len;
                fujinet_known = 1;
                Log_print("netsio: [ADDR] Remembered FujiNet address (len=%d)", (int)fujinet_addr_len);
                Log_print("netsio: Received first packet from FujiNet, remembering address");
                if (src_addr.ss_family == AF_INET) {
                    struct sockaddr_in *addr_in = (struct sockaddr_in *)&src_addr;
                    Log_print("netsio: FujiNet IP: %s:%d", 
                             inet_ntoa(addr_in->sin_addr), 
                             ntohs(addr_in->sin_port));
                }
            }
            
            Log_print("netsio: [PACKET] Received %s (0x%02X), length %zd, sync_wait=%d", 
                     cmd_name, cmd, n, netsio_sync_wait);
            
            /* Handle PING/PONG handshake */
            if (cmd == 0xC2) {  /* PING */
                Log_print("netsio: recv: PING→PONG");
                uint8_t pong = 0xC3;
                send_to_fujinet(&pong, 1);
                continue;
            }
            
            if (cmd == 0xC0) {  /* DISCONNECT */
                Log_print("netsio: recv: device disconnected");
                continue;
            }
            
            /* Dump first few bytes of packet for debugging */
            if (n > 1) {
                char hexdump[128] = {0};
                int pos = 0;
                for (int i = 0; i < n && i < 16 && pos < 120; i++) {
                    pos += snprintf(hexdump + pos, 128 - pos, "%02X ", buf[i]);
                }
                Log_print("netsio: [PACKET DATA] %s", hexdump);
            }

            if (netsio_sync_wait) {
                Log_print("netsio: [STATE] Currently in sync_wait state, expecting SYNC RESPONSE (0x81)");
            }
            
            switch (cmd) {
            case NETSIO_CREDIT_UPDATE: {
                Log_print("netsio: [SYNC] SYNC RESPONSE (0x81) received from FujiNet. netsio_sync_wait=%d", netsio_sync_wait);
                if (netsio_sync_wait) {
                    netsio_sync_wait = 0;
                    Log_print("netsio: [STATE] netsio_sync_wait CLEARED (now 0). Emulator no longer waiting for SYNC RESPONSE.");
                } else {
                    Log_print("netsio: [WARNING] Unexpected SYNC RESPONSE received - netsio_sync_wait was already 0!");
                }
                Log_print("netsio: [STATE] Current netsio_cmd_state=%d after SYNC RESPONSE.", netsio_cmd_state);
                break;
            }
            case NETSIO_ALIVE_REQUEST: {
                static int alive_counter = 0;
                alive_counter++;
                
                Log_print("netsio: [ALIVE] IT'S ALIVE! #%d (enabled=%d, cmd_state=%d, sync_wait=%d)",
                         alive_counter, netsio_enabled, netsio_cmd_state, netsio_sync_wait);
                
                if (netsio_sync_wait) {
                    Log_print("netsio: [WARNING] Received ALIVE while still waiting for SYNC RESPONSE!");
                }
                
                /* If we get too many alive messages in a row, it might indicate a problem */
                if (alive_counter > 20) {
                    Log_print("netsio: [WARNING] Received multiple IT'S ALIVE! messages. Check FujiNet connection.");
                    /* Force sync_wait to false to unstick the protocol */
                    if (netsio_sync_wait) {
                        Log_print("netsio: [STATE FORCE] Forcing netsio_sync_wait to 0 to unstick protocol");
                        netsio_sync_wait = 0;
                    }
                    alive_counter = 0;
                }
                
                uint8_t r = NETSIO_ALIVE_RESPONSE;
                send_to_fujinet(&r, 1);
                break;
            }

            case NETSIO_PING_REQUEST: {
                uint8_t r = NETSIO_PING_RESPONSE;
                send_to_fujinet(&r, 1);
                Log_print("netsio: recv: PING→PONG");
                break;
            }

            case NETSIO_DEVICE_CONNECTED: {
                Log_print("netsio: recv: device connected");
                /* give it some credits 
                uint8_t reply[2] = { NETSIO_CREDIT_UPDATE, 3 };
                send_to_fujinet(reply, sizeof(reply)); */
                netsio_enabled = 1;
                break;
            }

            case NETSIO_DEVICE_DISCONNECTED: {
                Log_print("netsio: recv: device disconnected");
                netsio_enabled = 0;
                break;
            }
            
            case NETSIO_CREDIT_STATUS: {
                /* packet should be 2 bytes long */
                if (n < 2) {
                    Log_print("netsio: recv: CREDIT_STATUS packet too short (%zd)", n);
                }
                uint8_t reply[2] = { NETSIO_CREDIT_UPDATE, 3 };
                send_to_fujinet(reply, sizeof(reply));
                Log_print("netsio: recv: credit status & response");
                break;
            }

            case NETSIO_SPEED_CHANGE: {
                /* packet: [cmd][baud32le] */
                if (n < 5) {
                    Log_print("netsio: recv: SPEED_CHANGE packet too short (%zd)", n);
                    break;
                }
                uint32_t baud = buf[1]
                              | (uint32_t)buf[2] << 8
                              | (uint32_t)buf[3] << 16
                              | (uint32_t)buf[4] << 24;
                Log_print("netsio: recv: requested baud rate %u", baud);
                /* TODO: apply baud */
                break;
            }

            case NETSIO_SYNC_RESPONSE: {
                Log_print("netsio: [PACKET] Received SYNC_ACK (0x81), length %zd, sync_wait=%d", n, netsio_sync_wait);
                Log_print("netsio: [PACKET DATA] %02X %02X %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
                if (netsio_sync_wait) {
                    Log_print("netsio: [STATE] Currently in sync_wait state, processing SYNC RESPONSE (0x81)");
                    uint8_t sync_num = buf[1];
                    uint8_t ack_type = buf[2];
                    uint8_t ack_byte = buf[3];
                    if (sync_num != netsio_sync_num - 1) {
                        Log_print("netsio: recv: sync-response: got %u, want %u",
                                  sync_num, netsio_sync_num - 1);
                    } else {
                        if (ack_type == 0) {
                            Log_print("netsio: recv: sync %u NAK, dropping", sync_num);
                        } else if (ack_type == 1) {
                            Log_print("netsio: recv: sync %u ACK byte=0x%02X",
                                      sync_num, ack_byte);
                            enqueue_to_emulator(&ack_byte, 1);
                        } else {
                            Log_print("netsio: recv: sync %u unknown ack_type %u",
                                      sync_num, ack_type);
                        }
                        /* netsio_next_write_size = write_size; */
                    }
                    netsio_sync_wait = 0; /* continue emulation */
                    netsio_cmd_state = 0; /* reset command state */
                    break;
                }
            }

            /* set_CA1 */
            case NETSIO_PROCEED_ON: {

                break;
            }
            case NETSIO_PROCEED_OFF: {

                break;
            }

            /* set_CB1 */
            case NETSIO_INTERRUPT_ON: {

                break;
            }
            case NETSIO_INTERRUPT_OFF: {

                break;
            }
            case NETSIO_DATA_BYTE: {
                /* packet: [cmd][data] */
                if (n < 2) {
                    Log_print("netsio: recv: DATA_BYTE too short (%zd)", n);
                    break;
                }
                uint8_t data = buf[1];
                Log_print("netsio: recv: data byte: 0x%02X", data);
                enqueue_to_emulator(&data, 1);
                break;
            }

            case NETSIO_DATA_BLOCK: {
                /* packet: [cmd][payload...] */
                if (n < 2) {
                    Log_print("netsio: recv: data block too short (%zd)", n);
                    break;
                }
                /* payload length is everything after the command byte */
                size_t payload_len = n - 1;
                Log_print("netsio: recv: data block %zu bytes:\n  %s", payload_len, buf_to_hex(buf, 1, payload_len));
                /* forward only buf[1]..buf[n-1] */
                enqueue_to_emulator(buf + 1, payload_len);
                break;
            }            

            default:
                Log_print("netsio: recv: unknown cmd 0x%02X, length %zd", cmd, n);
                break;
            }
        }
    }
    return NULL;
}

/* Thread: receive from emulator FIFO and send to FujiNet socket, disabled/not used now */
static void *emu_tx_thread(void *arg) {
    uint8_t buf[4096];
    size_t head = 0, tail = 0;
    uint8_t packet[65536];
    int i;

    for (;;) {
        ssize_t n = read(fds1[0], buf + tail, sizeof(buf) - tail);
        if (n <= 0) {
            perror("netsio: read from TX FIFO");
            /*exit(1);**/
        }
        tail += n;

        head = 0;
        while (head < tail) {
            uint8_t cmd = buf[head];
            size_t rem = tail - head;

            /* Handle COMMAND ON */
            if (cmd == NETSIO_COMMAND_ON) {
                uint8_t r = NETSIO_COMMAND_ON;
                Log_print("netsio: CMD ON");
                send_to_fujinet(&r, 1);
                head++;
                continue;
            }

            /* Handle COMMAND OFF */
            if (cmd == NETSIO_COMMAND_OFF) {
                uint8_t r = NETSIO_COMMAND_OFF;
                Log_print("netsio: CMD OFF");
                send_to_fujinet(&r, 1);
                head++;
                continue;
            }

            /* Handle COMMAND OFF SYNC */
            if (cmd == NETSIO_COMMAND_OFF_SYNC) {
                uint8_t r = NETSIO_COMMAND_OFF_SYNC;
                uint8_t b = 0x01;
                Log_print("netsio: CMD OFF SYNC");
                send_to_fujinet(&r, 1);
                send_to_fujinet(&b, 1); /* FIXME: send real incremented sync counter */
                head++;
                continue;
            }

            /* Handle other NETSIO frames */
            size_t pkt_len = 1;
            if ((cmd == NETSIO_DATA_BYTE) || (cmd == NETSIO_DATA_BYTE_SYNC)) {
                if (rem < 2) break;
                pkt_len = 2;
            } else if (cmd == NETSIO_DATA_BLOCK) {
                if (rem < 3) break;
                uint16_t L = buf[head+1] | (buf[head+2] << 8);
                if (rem < 3 + L) break;
                pkt_len = 3 + L;
            }

            memcpy(packet, buf + head, pkt_len);
            send_to_fujinet(packet, pkt_len);
            head += pkt_len;
        }

        if (head) {
            memmove(buf, buf + head, tail - head);
            tail -= head;
        }
    }
    return NULL;
}