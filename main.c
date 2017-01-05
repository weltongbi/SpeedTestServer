#include <netinet/in.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/tcp.h>
#include <signal.h>
#include "protocol.h"
#include <syslog.h>

void do_read(evutil_socket_t fd, short events, void *arg);
void do_write(evutil_socket_t fd, short events, void *arg);
void signal_cb(evutil_socket_t sig, short events, void *arg);

server_context *server_ctx;

void do_read(evutil_socket_t fd, short events, void *ctx){
    client *c = (client *)ctx;
    c->iddle = now();
    char *command = NULL;

    if (!client_get_state(c)){
        syslog(LOG_INFO, "%s:%s Quitting.", c->hoststr, c->portstr);
        client_free(c);
        evutil_closesocket(fd);
        return;
    }

    ssize_t result = 0;
    memset(c->buffer, 0, c->buffer_size);
    c->buffer_len = 0;
    while(true){
        result = recv(fd, c->buffer, c->buffer_size, 0);
        if (result <= 0)
            break;

        c->buffer_len = (size_t)result;
        c->iddle = now();
        if (c->request_upload_size_missing > 0){
            c->request_upload_size_missing -= result;
            if (c->request_upload_size_missing > 0)
                continue;

            upload_complete_handler(ctx);
            break;
        }


        command = parse_command(ctx);
        if (command != NULL){
            // Command received, getting ready to send back some data
//            syslog(LOG_DEBUG, "%s:%s CMD: %s", c->hoststr, c->portstr, command);
            event_add(c->write_event, NULL);
            if (strstr(command, "PING") == command){
                ping_handler(ctx);
            } else if (strstr(command, "HI") == command){
                hi_handler(ctx);
            } else if (strstr(command, "GETIP") == command) {
                getip_handler(ctx);
            } else if (strstr(command, "QUIT") == command) {
                quit_handler(ctx);
            } else if (strstr(command, "DOWNLOAD") == command){
                size_t download = 0;
                sscanf(command, "DOWNLOAD %zu", &download);
                download_request_handler(download, ctx);
            } else if (strstr(command, "UPLOAD") == command){
                size_t upload = 0;
                sscanf(command, "UPLOAD %zu", &upload);
                upload_request_handler(upload, c->buffer_len, ctx);
            } else {
                printf("Command not understood");
                error_handler(ctx);
            }
        }

        if (command != NULL)
            free(command);


        if (!client_get_state(c)){
            syslog(LOG_INFO, "%s:%s Quitting.", c->hoststr, c->portstr);
            client_free(c);
            evutil_closesocket(fd);
            return;
        }
    }


    if (result == 0) {
        client_free(c);
        evutil_closesocket(fd);
    } else if (result < 0) {
        if (errno == EAGAIN) // XXXX use evutil macro
            return;
        perror("recv");
        client_free(c);
        evutil_closesocket(fd);
    }


}

void do_write(evutil_socket_t fd, short events, void *ctx){
    client *c = (client *)ctx;
    c->iddle = now();
    if (!client_get_state(c)){
        client_free(c);
        evutil_closesocket(fd);
    }

    if (c->request_download_size > 0){
        download_execute_handler(ctx);
        return;
    }

    if (c->buffer_len > 0){
        ssize_t result = send(fd, c->buffer, c->buffer_len, 0);
        c->iddle = now();
        if (result == c->buffer_len){
            event_del(c->write_event);
        } else {
            if (errno == EAGAIN) // XXXX use evutil macro
                return;
            perror("send");
            syslog(LOG_ERR, "do_write error: %s. Disconnecting client", strerror(errno));
            client_free(c);
            evutil_closesocket(fd);
        }
    } else {
        event_del(c->write_event);
    }


}

void do_timeout(evutil_socket_t fd, short events, void *ctx){
    client *c = ctx;
    if (!client_get_state(c)){
        syslog(LOG_INFO, "%s:%s Quitting.", c->hoststr, c->portstr);
        client_free(c);
        evutil_closesocket(fd);
        return;
    }
    size_t time_diff = (now() - c->iddle) / 1000;
    if (time_diff >= c->srv_ctx->config->idle_timeout){
        syslog(LOG_INFO, "%s:%s Idle timeout [%zu]. ", c->hoststr, c->portstr, time_diff);
        client_free(c);
        evutil_closesocket(fd);
    }
}

void do_accept(evutil_socket_t listener, short event, void *arg) {
    struct event_base *base = arg;
    struct sockaddr_storage ss;
    socklen_t socklen = sizeof(ss);

    int fd = accept(listener, (struct sockaddr*)&ss, &socklen);
    if (fd < 0) {
        perror("accept");
    } else if (fd > FD_SETSIZE) {
        close(fd);
    } else {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        client *c = client_new(base, fd, server_ctx, do_read, do_write, do_timeout);
        c->connected_at = now();
        c->iddle = c->connected_at;


        getnameinfo((struct sockaddr *)&ss,
                    socklen,
                    c->hoststr,
                    sizeof(c->hoststr),
                    c->portstr,
                    sizeof(c->portstr),
                    NI_NUMERICHOST | NI_NUMERICSERV
        );

        c->srv_ctx->total_client++;
        c->srv_ctx->current_connected_client++;

        syslog(LOG_INFO, "New client: %s:%s", c->hoststr, c->portstr);
        evutil_make_socket_nonblocking(fd);
        event_add(c->read_event, NULL);
    }
}

void signal_cb(evutil_socket_t sig, short events, void *arg){
    struct event_base *base = arg;
    event_base_loopexit(base, NULL);

}


void run() {
    evutil_socket_t listener;
    struct sockaddr_in sin;
    struct event_base *base;
    struct event *listener_event;
    struct event *signal_event;

    base = event_base_new();
    if (!base)
        return;

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(server_ctx->config->tcp_port);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    evutil_make_socket_nonblocking(listener);

    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));


    if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        perror("bind");
        return;
    }

    if (listen(listener, server_ctx->config->backlog) < 0) {
        perror("listen");
        return;
    }


    listener_event = event_new(base, listener, EV_READ|EV_PERSIST, do_accept, (void*)base);
    signal_event  = evsignal_new(base, SIGINT, signal_cb, (void*)base);
    event_add(signal_event, NULL);
    event_add(listener_event, NULL);

    syslog(LOG_INFO, "Ready to accept connections");

    event_base_dispatch(base);
    shutdown(listener, SHUT_RD);
    close(listener);

}

int main(const int argc, char **argv) {
    openlog("SpeedTestServer", LOG_PID|LOG_CONS|LOG_PERROR, LOG_USER);
    protocol_config *speed_test_proto_config = protocol_config_new();

    if (speed_test_proto_config == NULL){
        return EXIT_FAILURE;
    }


    speed_test_proto_config->download_max = 100000000; // 100Mb
    speed_test_proto_config->upload_max   = 100000000; // 100Mb
    speed_test_proto_config->idle_timeout = 10; // 10 sec
    speed_test_proto_config->backlog = 512;
    speed_test_proto_config->max_client_error = 3;
    speed_test_proto_config->tcp_port = 5060;


    server_ctx = server_context_new(speed_test_proto_config);

    if (server_ctx == NULL){
        return EXIT_FAILURE;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    syslog(LOG_INFO,
           "Starting SpeedTestServer on TCP port %d with a backlog of %d",
           speed_test_proto_config->tcp_port,
           speed_test_proto_config->backlog
    );
    run();
    protocol_config_free(speed_test_proto_config);
    server_context_free(server_ctx);
    closelog();
    return EXIT_SUCCESS;
}