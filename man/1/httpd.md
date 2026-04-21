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

## CURRENT CAPABILITIES

- serve files from a chosen document root
- handle GET and HEAD requests
- reject directory listing
- reject simple path traversal attempts and symlink escape outside the chosen root
- expose lightweight health endpoints at /health and /_status
- bound the number of concurrent connections
- close idle connections after a configured timeout
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

- bind
- port
- root
- index
- max_connections
- idle_timeout
- quiet

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
