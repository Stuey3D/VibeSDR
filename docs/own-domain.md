# Your own domain, with HTTPS

VibeServer speaks plain HTTP on one port (48000 unless you changed it). It has no TLS of its own, so for
`https://` you put something in front of it that does: Caddy, nginx, or your own Cloudflare. Then list the
server on VibeSDR.net with **My own domain with HTTPS** on the setup page.

If you don't need your own name, choose **Automatic** instead. It's free, there's nothing to set up, it works
behind CGNAT, and you get `name.vibeserver.vibesdr.net` with HTTPS included.

## The rules

- **Point the proxy at the main port only**: `127.0.0.1:48000`, or the server's LAN address if the proxy
  runs on another machine. Each radio's own port (48001, 48002 and so on) answers on the server machine only.
  Every listener comes in through 48000, and the server hands each one to the right radio.
- **Forward 443 (and 80, for the certificate) to the proxy, not 48000.** If you have no proxy and forward
  48000 itself, you get a plain `http://` address. That works, but browsers will call it "not secure".
- **Serve it at the root of a host**: `https://sdr.example.org`, not `https://example.org/sdr`. The directory
  keeps only the scheme, host and port of the address you give it.
- **Let WebSockets through.** The audio, the waterfall and the controls all run over WebSockets
  (`/ws`, `/r/<radio>/…`). A proxy that drops the `Upgrade` header gives you a page that loads and never
  plays.

## Caddy

Caddy gets the certificate and passes WebSockets on by itself:

```
sdr.example.org {
    reverse_proxy 127.0.0.1:48000
}
```

## nginx

```
server {
    listen 443 ssl;
    http2 on;
    server_name sdr.example.org;
    ssl_certificate     /etc/letsencrypt/live/sdr.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/sdr.example.org/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:48000;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_buffering off;          # audio and waterfall are a live stream
        proxy_read_timeout 1h;        # a quiet socket is not a dead one
        proxy_send_timeout 1h;
    }
}

# in the http { } block:
map $http_upgrade $connection_upgrade { default upgrade; '' close; }
```

## Trusted proxy addresses

Behind a proxy, every listener seems to come from the proxy's address. One ban would block everybody, everyone
would show the same country, and one wrong admin password could lock out the world. Put the proxy's address in
**Behind a reverse proxy → Trusted proxy addresses** on the setup page. Use `127.0.0.1, ::1` if it runs on the
same machine, or its LAN address if it doesn't. The server then reads the real visitor from `X-Forwarded-For`.
Leave the box empty if there is no proxy: the header is just text, and anyone could write it.

If your own Cloudflare (the orange cloud) sits in front of your proxy, the proxy sees Cloudflare's address, not
the visitor's. Have the proxy restore the visitor first. In nginx that's `real_ip_header CF-Connecting-IP;` with
a `set_real_ip_from` line for each of Cloudflare's published ranges.

## Listing it on VibeSDR.net

On the setup page, under **Advertise on VibeSDR.net**:

1. Switch on **List this server publicly**.
2. Choose **My own domain with HTTPS** and type `https://sdr.example.org`.
3. Choose whether it **also** gets a `name.vibeserver.vibesdr.net` address. It's on by default. With it on,
   the directory page links to that address, which also lets the page show live listener counts. With it
   off, the listing links straight to your domain, and the vibesdr.net name answers "not found". Your name
   stays reserved, so you can turn the address back on later and get the same one.
4. Apply and restart.

### What the directory checks

When the server registers, and again every 15 minutes, the directory fetches
`https://sdr.example.org/vibeserver.json?dirNonce=…` from the internet. The server must answer within
15 seconds with proof signed by its own listing key. Only a server that answers is shown on the map. If it
stops answering, the listing drops off until it answers again.

So the address must work **from outside your network**. Check from a phone on mobile data. Private and local
addresses (`192.168.…`, `10.…`, `.local`, `localhost`) are refused outright.
