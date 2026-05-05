#include "MetalXRProtocol/metalxr_protocol.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int make_loopback_pair(int sockets[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return 0;
    }
    return 1;
}

static MetalXRHelloPayload make_hello(uint32_t role, const char* name)
{
    MetalXRHelloPayload hello;
    memset(&hello, 0, sizeof(hello));
    hello.role = role;
    hello.capabilities = METALXR_CAPABILITY_H264 |
                         METALXR_CAPABILITY_STEREO_SEPARATE_EYES |
                         METALXR_CAPABILITY_POSE_INPUT |
                         METALXR_CAPABILITY_CONTROLLER_INPUT |
                         METALXR_CAPABILITY_LOG_STREAM;
    hello.maxVideoWidth = 1832;
    hello.maxVideoHeight = 1920;
    hello.preferredFps = 90;
    hello.controlPort = 47000;
    hello.mediaPort = 47001;
    snprintf(hello.deviceName, sizeof(hello.deviceName), "%s", name);
    return hello;
}

static int send_error(int fd, uint64_t sequence, const MetalXRErrorPayload* errorPayload)
{
    MetalXRPacketHeader errorHeader = metalxr_make_header(
        METALXR_PACKET_ERROR,
        sequence,
        metalxr_protocol_now_ns(),
        sizeof(*errorPayload));
    return metalxr_protocol_send_packet(fd, &errorHeader, errorPayload);
}

static int server_accept_hello(int fd, uint64_t sessionId)
{
    MetalXRPacketHeader header;
    MetalXRHelloPayload hello;
    if (!metalxr_protocol_recv_packet(fd, &header, &hello, sizeof(hello))) {
        fprintf(stderr, "server failed to receive hello\n");
        return 0;
    }

    MetalXRErrorPayload errorPayload;
    if (!metalxr_protocol_validate_header(&header, sizeof(hello), &errorPayload) ||
        header.type != METALXR_PACKET_HELLO) {
        if (errorPayload.code == METALXR_ERROR_NONE) {
            errorPayload.code = METALXR_ERROR_BAD_PACKET;
            snprintf(errorPayload.message,
                     sizeof(errorPayload.message),
                     "Expected HELLO, got %s",
                     metalxr_packet_type_name(header.type));
        }
        (void)send_error(fd, header.sequence, &errorPayload);
        printf("server rejected hello: %s %s\n",
               metalxr_error_code_name(errorPayload.code),
               errorPayload.message);
        return 0;
    }

    printf("server received hello: role=%u name=%s caps=0x%08x max=%ux%u fps=%u\n",
           hello.role,
           hello.deviceName,
           hello.capabilities,
           hello.maxVideoWidth,
           hello.maxVideoHeight,
           hello.preferredFps);

    MetalXRHelloPayload ack = make_hello(METALXR_ROLE_HOST, "MetalXR Mac Host");
    ack.reserved = (uint32_t)sessionId;
    MetalXRPacketHeader ackHeader = metalxr_make_header(
        METALXR_PACKET_HELLO_ACK,
        header.sequence + 1,
        metalxr_protocol_now_ns(),
        sizeof(ack));
    return metalxr_protocol_send_packet(fd, &ackHeader, &ack);
}

static int client_expect_hello_ack(int fd)
{
    MetalXRPacketHeader header;
    MetalXRHelloPayload ack;
    if (!metalxr_protocol_recv_packet(fd, &header, &ack, sizeof(ack))) {
        fprintf(stderr, "client failed to receive hello ack\n");
        return 0;
    }

    MetalXRErrorPayload errorPayload;
    if (!metalxr_protocol_validate_header(&header, sizeof(ack), &errorPayload) ||
        header.type != METALXR_PACKET_HELLO_ACK) {
        fprintf(stderr, "client expected HELLO_ACK, got %s: %s\n",
                metalxr_packet_type_name(header.type),
                errorPayload.message);
        return 0;
    }

    printf("client received hello ack: role=%u name=%s caps=0x%08x\n",
           ack.role,
           ack.deviceName,
           ack.capabilities);
    return 1;
}

static int exchange_heartbeat(int clientFd, int serverFd, uint64_t sessionId)
{
    MetalXRHeartbeatPayload heartbeat;
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.sessionId = sessionId;
    heartbeat.monotonicTimeNs = metalxr_protocol_now_ns();
    heartbeat.lastFrameId = 42;
    heartbeat.batteryPercent = 100;

    MetalXRPacketHeader heartbeatHeader = metalxr_make_header(
        METALXR_PACKET_HEARTBEAT,
        10,
        heartbeat.monotonicTimeNs,
        sizeof(heartbeat));
    if (!metalxr_protocol_send_packet(clientFd, &heartbeatHeader, &heartbeat)) {
        return 0;
    }

    MetalXRPacketHeader serverHeader;
    MetalXRHeartbeatPayload serverHeartbeat;
    if (!metalxr_protocol_recv_packet(serverFd, &serverHeader, &serverHeartbeat, sizeof(serverHeartbeat))) {
        return 0;
    }

    MetalXRErrorPayload errorPayload;
    if (!metalxr_protocol_validate_header(&serverHeader, sizeof(serverHeartbeat), &errorPayload) ||
        serverHeader.type != METALXR_PACKET_HEARTBEAT) {
        fprintf(stderr, "server expected HEARTBEAT: %s\n", errorPayload.message);
        return 0;
    }

    printf("server received heartbeat: session=%llu lastFrame=%llu time=%llu\n",
           (unsigned long long)serverHeartbeat.sessionId,
           (unsigned long long)serverHeartbeat.lastFrameId,
           (unsigned long long)serverHeartbeat.monotonicTimeNs);

    serverHeartbeat.monotonicTimeNs = metalxr_protocol_now_ns();
    serverHeartbeat.lastFrameId = 43;
    MetalXRPacketHeader replyHeader = metalxr_make_header(
        METALXR_PACKET_HEARTBEAT,
        serverHeader.sequence + 1,
        serverHeartbeat.monotonicTimeNs,
        sizeof(serverHeartbeat));
    if (!metalxr_protocol_send_packet(serverFd, &replyHeader, &serverHeartbeat)) {
        return 0;
    }

    MetalXRPacketHeader clientHeader;
    MetalXRHeartbeatPayload clientHeartbeat;
    if (!metalxr_protocol_recv_packet(clientFd, &clientHeader, &clientHeartbeat, sizeof(clientHeartbeat))) {
        return 0;
    }

    if (!metalxr_protocol_validate_header(&clientHeader, sizeof(clientHeartbeat), &errorPayload) ||
        clientHeader.type != METALXR_PACKET_HEARTBEAT) {
        fprintf(stderr, "client expected HEARTBEAT: %s\n", errorPayload.message);
        return 0;
    }

    printf("client received heartbeat: session=%llu lastFrame=%llu time=%llu\n",
           (unsigned long long)clientHeartbeat.sessionId,
           (unsigned long long)clientHeartbeat.lastFrameId,
           (unsigned long long)clientHeartbeat.monotonicTimeNs);
    return 1;
}

static int run_successful_loopback(void)
{
    int sockets[2] = { -1, -1 };
    if (!make_loopback_pair(sockets)) {
        return 0;
    }

    const uint64_t sessionId = 0x4d58525345535301ull;
    MetalXRHelloPayload clientHello = make_hello(METALXR_ROLE_QUEST_CLIENT, "Quest Loopback Client");
    MetalXRPacketHeader helloHeader = metalxr_make_header(
        METALXR_PACKET_HELLO,
        1,
        metalxr_protocol_now_ns(),
        sizeof(clientHello));
    if (!metalxr_protocol_send_packet(sockets[0], &helloHeader, &clientHello) ||
        !server_accept_hello(sockets[1], sessionId) ||
        !client_expect_hello_ack(sockets[0]) ||
        !exchange_heartbeat(sockets[0], sockets[1], sessionId)) {
        close(sockets[0]);
        close(sockets[1]);
        return 0;
    }

    close(sockets[0]);
    close(sockets[1]);
    return 1;
}

static int run_version_mismatch_loopback(void)
{
    int sockets[2] = { -1, -1 };
    if (!make_loopback_pair(sockets)) {
        return 0;
    }

    MetalXRHelloPayload clientHello = make_hello(METALXR_ROLE_QUEST_CLIENT, "Old Quest Client");
    MetalXRPacketHeader helloHeader = metalxr_make_header(
        METALXR_PACKET_HELLO,
        100,
        metalxr_protocol_now_ns(),
        sizeof(clientHello));
    helloHeader.versionMajor = METALXR_PROTOCOL_VERSION_MAJOR + 1;
    if (!metalxr_protocol_send_packet(sockets[0], &helloHeader, &clientHello)) {
        close(sockets[0]);
        close(sockets[1]);
        return 0;
    }

    if (server_accept_hello(sockets[1], 0x1)) {
        fprintf(stderr, "version mismatch was accepted unexpectedly\n");
        close(sockets[0]);
        close(sockets[1]);
        return 0;
    }

    MetalXRPacketHeader errorHeader;
    MetalXRErrorPayload errorPayload;
    if (!metalxr_protocol_recv_packet(sockets[0], &errorHeader, &errorPayload, sizeof(errorPayload))) {
        fprintf(stderr, "client failed to receive version mismatch error\n");
        close(sockets[0]);
        close(sockets[1]);
        return 0;
    }

    if (errorHeader.type != METALXR_PACKET_ERROR ||
        errorPayload.code != METALXR_ERROR_VERSION_MISMATCH) {
        fprintf(stderr, "unexpected mismatch response: type=%s code=%s\n",
                metalxr_packet_type_name(errorHeader.type),
                metalxr_error_code_name(errorPayload.code));
        close(sockets[0]);
        close(sockets[1]);
        return 0;
    }

    printf("client received expected error: %s %s\n",
           metalxr_error_code_name(errorPayload.code),
           errorPayload.message);

    close(sockets[0]);
    close(sockets[1]);
    return 1;
}

int main(void)
{
    if (!run_successful_loopback()) {
        return 1;
    }

    if (!run_version_mismatch_loopback()) {
        return 1;
    }

    printf("MetalXR protocol loopback probe passed\n");
    return 0;
}
