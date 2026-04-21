# HTTPD

## NAME

httpd - small static HTTP server for hosted newos development and service bring-up

## SYNOPSIS

httpd [-q] [-b HOST] [-p PORT] [-r ROOT] [-i INDEX] [-m MAX]
      [-t TIMEOUT] [-c CONFIG]

## DESCRIPTION

httpd serves a bounded document tree over plain HTTP using one process, a
poll-style event loop, fixed connection limits, and a strict version-one scope.
It is intentionally a small static file server rather than a general web
framework.

The current implementation is aimed at hosted POSIX and macOS development and
pairs naturally with the small service supervisor.

Direct foreground execution is supported for local debugging and bring-up.
For longer-running hosted deployment, the recommended path is to run it through
the service supervisor with a repository-local config and log layout under the
services tree.

## CURRENT CAPABILITIES

- serve files from a chosen document root
- handle GET and HEAD requests
- reject directory listing
- reject simple path traversal attempts and symlink escape outside the chosen root
- refuse dotfiles and other hidden path components under the served tree
- reject malformed requests that do not present a proper HTTP/1.0 or HTTP/1.1 request line
- expose lightweight health endpoints at /health and /_status
- bound the number of concurrent connections
- close idle connections after a configured timeout
- emit conservative hardening headers such as nosniff and frame denial
- optionally drop privileges after opening the listening socket when user or group is configured in the daemon config
- load a small key/value config file

## OPTIONS

- -b HOST, --bind HOST - bind to HOST; defaults to 127.0.0.1
- -p PORT, --port PORT - listen on PORT; defaults to 8080
- -r ROOT, --root ROOT - serve files below ROOT
- -i INDEX, --index INDEX - use INDEX as the default file for /
- -m MAX, --max-connections MAX - cap the active connection table
- -t TIMEOUT, --idle-timeout TIMEOUT - idle connection timeout; accepts values such as 500ms, 2s, or 1m
- -c CONFIG, --config CONFIG - read settings from a small key=value config file
- -q, --quiet - reduce informational request logging
- -h, --help - print the usage summary

## CONFIG FILE

The config file uses a simple key=value format. Supported keys today are:

- bind - local address to bind, for example 127.0.0.1 or 0.0.0.0
- port - TCP port number to listen on
- root - document root directory that will be served
- index - default file name to use for / and directory-style paths
- max_connections - size of the in-process connection table
- idle_timeout - per-connection idle timeout using duration syntax such as 500ms or 5s
- quiet - true or false to reduce informational logging
- user - optional target user name or numeric uid to switch to after the listener is opened
- group - optional target group name or numeric gid to switch to after the listener is opened

This post-bind drop model is appropriate for daemon-style execution when the process needs to open its listener first and then continue in a reduced-privilege state.

For hosted deployments, keep root pointed at a public content directory such as a tree under services/httpd/www-root and keep config and log files outside that served tree.

## LIMITATIONS

- plain HTTP only; HTTPS and TLS are still separate future work
- static files and fixed health routes only
- no CGI, FastCGI, plugin model, or directory listing
- no range requests, upload handling, or full web-platform compatibility
- intended for small and auditable local or controlled deployments

## EXAMPLES

- httpd -r ./public -p 8080
- httpd -b 0.0.0.0 -p 8000 -r /srv/www
- httpd -c ./httpd.conf

## SEE ALSO

service, wget, netcat, server
