#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SS_MAX_SOCKETS 4096U

static int show_tcp = 0;
static int show_udp = 0;
static int show_listen = 0;
static int ss_json;

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static void print_usage(void) {
    tool_write_usage("ss", "[-t] [-u] [-l] [-a] [--json]");
}

static int write_json_socket(const PlatformSocketEntry *socket) {
    if (tool_json_begin_event(1, "ss", "stdout", "socket") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"protocol\":") != 0) return -1;
    if (tool_json_write_string(1, socket->protocol) != 0) return -1;
    if (rt_write_cstr(1, ",\"state\":") != 0) return -1;
    if (tool_json_write_string(1, socket->state) != 0) return -1;
    if (rt_write_cstr(1, ",\"local_address\":") != 0) return -1;
    if (tool_json_write_string(1, socket->local_address) != 0) return -1;
    if (rt_write_cstr(1, ",\"local_port\":") != 0) return -1;
    if (rt_write_uint(1, socket->local_port) != 0) return -1;
    if (rt_write_cstr(1, ",\"remote_address\":") != 0) return -1;
    if (tool_json_write_string(1, socket->remote_address) != 0) return -1;
    if (rt_write_cstr(1, ",\"remote_port\":") != 0) return -1;
    if (rt_write_uint(1, socket->remote_port) != 0) return -1;
    if (rt_write_cstr(1, ",\"inode\":") != 0) return -1;
    if (rt_write_uint(1, socket->inode) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static void write_padding(size_t width, size_t used) {
    while (used < width) {
        rt_write_char(1, ' ');
        used += 1U;
    }
}

int main(int argc, char **argv) {
    PlatformSocketEntry sockets[SS_MAX_SOCKETS];
    size_t count = 0U;
    size_t i;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (streq(argv[argi], "-t") || streq(argv[argi], "--tcp")) {
            show_tcp = 1;
        } else if (streq(argv[argi], "-u") || streq(argv[argi], "--udp")) {
            show_udp = 1;
        } else if (streq(argv[argi], "-l") || streq(argv[argi], "--listening")) {
            show_listen = 1;
        } else if (streq(argv[argi], "-a") || streq(argv[argi], "--all")) {
            show_listen = 0;
        } else if (streq(argv[argi], "--json")) {
            ss_json = 1;
            tool_json_set_enabled(1);
        } else if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_usage();
            return 0;
        } else {
            tool_write_error("ss", "unknown option: ", argv[argi]);
            return 1;
        }
    }
    if (!show_tcp && !show_udp) {
        show_tcp = 1;
        show_udp = 1;
    }
    if (platform_list_sockets(sockets, SS_MAX_SOCKETS, &count, show_tcp, show_udp, show_listen) != 0) {
        tool_write_error("ss", "socket listing is not supported on this platform", 0);
        return 1;
    }
    if (!ss_json) {
        rt_write_line(1, "Netid State      Local Address:Port        Peer Address:Port         Inode");
    }
    for (i = 0U; i < count; ++i) {
        size_t used;

        if (ss_json) {
            if (write_json_socket(&sockets[i]) != 0) {
                return 1;
            }
            continue;
        }
        rt_write_cstr(1, sockets[i].protocol);
        write_padding(6U, rt_strlen(sockets[i].protocol));
        rt_write_cstr(1, sockets[i].state);
        write_padding(11U, rt_strlen(sockets[i].state));
        used = rt_strlen(sockets[i].local_address) + 1U;
        rt_write_cstr(1, sockets[i].local_address);
        rt_write_char(1, ':');
        rt_write_uint(1, sockets[i].local_port);
        used += sockets[i].local_port >= 10000U ? 5U : sockets[i].local_port >= 1000U ? 4U : sockets[i].local_port >= 100U ? 3U : sockets[i].local_port >= 10U ? 2U : 1U;
        write_padding(26U, used);
        used = rt_strlen(sockets[i].remote_address) + 1U;
        rt_write_cstr(1, sockets[i].remote_address);
        rt_write_char(1, ':');
        rt_write_uint(1, sockets[i].remote_port);
        used += sockets[i].remote_port >= 10000U ? 5U : sockets[i].remote_port >= 1000U ? 4U : sockets[i].remote_port >= 100U ? 3U : sockets[i].remote_port >= 10U ? 2U : 1U;
        write_padding(26U, used);
        rt_write_uint(1, sockets[i].inode);
        rt_write_char(1, '\n');
    }
    return 0;
}
