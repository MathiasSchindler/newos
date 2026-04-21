# SERVICE

## NAME

service - small config-driven service supervisor for start, stop, restart, and status

## SYNOPSIS

service start CONFIG
service stop CONFIG
service restart CONFIG
service status CONFIG

## DESCRIPTION

service manages a single long-running program described by a small repository-local
config file. It is intentionally modest in scope: it tracks a pidfile, starts a
background process, sends stop signals, and reports status.

This is suitable for project-local daemons such as httpd and other small network
services. It is not intended to replace a full system init or dependency-aware
service manager.

Multiple service-managed daemon instances are supported today as long as each
instance uses its own config file, pidfile, log paths, and listening port.

## CONFIG FILE

The config file uses simple key=value lines. Supported keys today are:

- command - the command line to execute; it must use an explicit path rather than relying on PATH lookup
- pidfile - path to the pid file to maintain
- stdout - append stdout and stderr to this path
- stderr - fallback log path if stdout is not set
- workdir - optional working directory to enter before exec so relative paths resolve predictably
- user - optional target user name or numeric uid for a pre-exec privilege drop
- group - optional target group name or numeric gid for a pre-exec privilege drop
- stop_timeout - graceful stop timeout before escalation

When user or group is configured, the service supervisor drops privileges in the child before executing the daemon.

For safer hosted operation, pidfile and log targets must resolve through existing directories and may not point at symlinks.

## CURRENT CAPABILITIES

- start a configured daemon in the background
- require an explicit executable path to avoid accidental or malicious PATH-based command hijacking
- optionally change into a configured working directory before exec
- optionally drop to a configured user and group before exec on hosted POSIX and freestanding Linux builds
- create pidfiles exclusively and refuse stale or mismatched process identities more defensively during status, stop, and restart
- fail the start operation if the child exits immediately during early setup, such as when a port is already in use
- stop it with a polite TERM signal followed by a bounded wait
- escalate to KILL if the process ignores the stop timeout
- report running or stopped state from the pidfile and process table
- restart a service by combining stop and start

## LIMITATIONS

- supervises one service definition at a time
- no dependency ordering or socket activation
- no launchd or systemd unit generation yet
- stderr currently follows stdout when both are redirected

## EXAMPLES

- service start ./httpd.conf
- service status ./httpd.conf
- service restart ./httpd.conf
- service stop ./httpd.conf

## SEE ALSO

httpd, init, server
