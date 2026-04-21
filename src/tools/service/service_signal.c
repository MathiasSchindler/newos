#include "service_impl.h"

#include "platform.h"

int service_stop_process(const ServiceConfig *config, int pid, const char *expected_name) {
    unsigned long long waited = 0ULL;

    if (config == NULL || pid <= 0) {
        return -1;
    }

    (void)platform_send_signal(pid, 15);
    while (waited < config->stop_timeout_ms) {
        if (!service_pid_is_running(pid, expected_name)) {
            (void)service_remove_pidfile(config->pidfile);
            return 0;
        }
        (void)platform_sleep_milliseconds(50U);
        waited += 50U;
    }

    (void)platform_send_signal(pid, 9);
    waited = 0ULL;
    while (waited < 1000ULL) {
        if (!service_pid_is_running(pid, expected_name)) {
            (void)service_remove_pidfile(config->pidfile);
            return 0;
        }
        (void)platform_sleep_milliseconds(50U);
        waited += 50U;
    }

    return -1;
}
