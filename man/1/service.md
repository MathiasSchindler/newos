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

## CONFIG FILE

The config file uses simple key=value lines. Supported keys today are:

- command - the command line to execute
- pidfile - path to the pid file to maintain
- stdout - append stdout and stderr to this path
- stderr - fallback log path if stdout is not set
- workdir - reserved for future use
- stop_timeout - graceful stop timeout before escalation

## CURRENT CAPABILITIES

- start a configured daemon in the background
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
