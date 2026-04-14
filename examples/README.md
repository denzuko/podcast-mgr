# examples/

Server configuration examples for deploying `index.cgi` as a FastCGI worker.

| File | Web server | Notes |
|------|-----------|-------|
| `nginx.conf` | nginx | `fastcgi_pass` to Unix socket via fcgiwrap or spawn-fcgi |
| `haproxy.cfg` | HAProxy 2.4+ | Native `fcgi-app` directive |
| `fcgiwrap.conf` | any | Socket/process management: systemd, FreeBSD rc.d, spawn-fcgi |
| `.htaccess` | Apache httpd | mod_fcgid (preferred) or mod_fastcgi (legacy) |

## Common conventions

All examples use the socket path `/var/run/podcast-mgr/fcgi.sock`.
Adjust to match your host's layout.

The FastCGI worker must be able to read and write:

```
$HOME/.config/podcasts/feeds.xml
$HOME/.config/podcasts/feeds.xml.tmp   (created during writes)
```

Set `HOME` (or `XDG_CONFIG_HOME`) in the environment block of whichever
config you use so `index.cgi` can find its configuration.

## Quick start with spawn-fcgi + nginx

```sh
# 1. Build
cc -o nob nob.c && ./nob

# 2. Create socket directory
install -d -m 750 /var/run/podcast-mgr

# 3. Start worker
spawn-fcgi -s /var/run/podcast-mgr/fcgi.sock -M 0660 \
    -u $(whoami) -g www -n -- ./index.cgi

# 4. Drop examples/nginx.conf into /etc/nginx/conf.d/ and reload
nginx -s reload
```
