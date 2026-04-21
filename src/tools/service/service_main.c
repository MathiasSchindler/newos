#include "service_impl.h"

#include "runtime.h"
#include "tool_util.h"

static void service_print_usage(const char *program_name) {
    tool_write_usage(program_name, "start|stop|restart|status CONFIG");
}

static void service_write_message(const char *message, int pid) {
    rt_write_cstr(1, "service: ");
    rt_write_cstr(1, message);
    if (pid > 0) {
        rt_write_cstr(1, " ");
        rt_write_uint(1, (unsigned long long)pid);
    }
    rt_write_char(1, '\n');
}

int service_main(int argc, char **argv) {
    ServiceConfig config;
    const char *command;
    const char *config_path;
    int pid = -1;

    if (argc < 2 || rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0) {
        service_print_usage(tool_base_name(argv[0]));
        rt_write_line(1, "Manage a small supervised background service from a repository-local config file.");
        return argc < 2 ? 1 : 0;
    }
    if (argc < 3) {
        service_print_usage(tool_base_name(argv[0]));
        return 1;
    }

    command = argv[1];
    config_path = argv[2];
    if (service_load_config(config_path, &config) != 0) {
        tool_write_error("service", "failed to load config: ", config_path);
        return 1;
    }

    if (rt_strcmp(command, "start") == 0) {
        if (service_read_pidfile(config.pidfile, &pid) == 0 && service_pid_is_running(pid)) {
            service_write_message("already running", pid);
            return 0;
        }
        if (service_start_process(&config, &pid) != 0) {
            tool_write_error("service", "failed to start service", 0);
            return 1;
        }
        service_write_message("started", pid);
        return 0;
    }

    if (rt_strcmp(command, "stop") == 0) {
        if (service_read_pidfile(config.pidfile, &pid) != 0 || !service_pid_is_running(pid)) {
            (void)service_remove_pidfile(config.pidfile);
            service_write_message("stopped", -1);
            return 0;
        }
        if (service_stop_process(&config, pid) != 0) {
            tool_write_error("service", "failed to stop service", 0);
            return 1;
        }
        service_write_message("stopped", -1);
        return 0;
    }

    if (rt_strcmp(command, "restart") == 0) {
        if (service_read_pidfile(config.pidfile, &pid) == 0 && service_pid_is_running(pid)) {
            if (service_stop_process(&config, pid) != 0) {
                tool_write_error("service", "failed to stop service for restart", 0);
                return 1;
            }
        }
        if (service_start_process(&config, &pid) != 0) {
            tool_write_error("service", "failed to restart service", 0);
            return 1;
        }
        service_write_message("started", pid);
        return 0;
    }

    if (rt_strcmp(command, "status") == 0) {
        if (service_read_pidfile(config.pidfile, &pid) == 0 && service_pid_is_running(pid)) {
            service_write_message("running", pid);
            return 0;
        }
        (void)service_remove_pidfile(config.pidfile);
        service_write_message("stopped", -1);
        return 1;
    }

    tool_write_error("service", "unknown action: ", command);
    service_print_usage(tool_base_name(argv[0]));
    return 1;
}
