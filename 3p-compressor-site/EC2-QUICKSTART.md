# EC2 quick start (Amazon Linux 2023)

Upload `3p-compressor-site-ec2-ready.zip` to `/var/www/website`, then update the extracted project:

```bash
cd /var/www/website
unzip -o 3p-compressor-site-ec2-ready.zip
cd /var/www/website/3p-compressor-site
```

The archive excludes `.env`, so existing SMTP secrets are not overwritten. Run these commands from the project directory:

```bash
cd /var/www/website/3p-compressor-site
sudo dnf install -y nodejs24 nodejs24-npm gcc-c++ make nginx
npm-24 install --omit=dev
g++ -std=c++17 -O3 -pthread p3cli.cpp -o p3cli
chmod 755 p3cli
```

Linux does **not** use the Windows `.bat` files. The online compressor runs the `p3cli` binary built above.

Create/configure the private environment file:

```bash
cp .env.example .env   # skip if .env already exists
nano .env
chmod 600 .env
```

For SES standard outbound SMTP, set `SMTP_HOST=email-smtp.<region>.amazonaws.com`, `SMTP_PORT=587`, `SMTP_SECURE=false`, the Region-specific SES SMTP username/password, `SMTP_FROM` to a verified sender, and `CONTACT_TO=thisispankajkc@gmail.com`. Do not use AWS access keys as SMTP credentials. A Mail Manager `mail-manager-smtp` ingress endpoint needs a rule set that routes the message to the recipient. Never commit or share `.env` or its values.

Run the site under systemd by creating `/etc/systemd/system/3p-compressor.service`:

```ini
[Unit]
Description=3P Compressor website
After=network.target

[Service]
Type=simple
User=ec2-user
WorkingDirectory=/var/www/website/3p-compressor-site
EnvironmentFile=/var/www/website/3p-compressor-site/.env
ExecStart=/usr/bin/node-24 /var/www/website/3p-compressor-site/server.js
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Then run:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now 3p-compressor
sudo systemctl restart 3p-compressor
sudo systemctl status 3p-compressor --no-pager
sudo journalctl -u 3p-compressor -n 80 --no-pager
sudo nginx -t && sudo systemctl reload nginx
```

The Nginx site should proxy to `http://127.0.0.1:3000` and allow request bodies up to `100m` (or your chosen `MAX_UPLOAD_MB`). Permit inbound HTTP/HTTPS in the EC2 security group. If the contact form still says delivery is not configured, the running service is not loading the `.env`; check its `EnvironmentFile` and restart it. If it reports another SMTP error, inspect `journalctl` and remove secrets before sharing logs.
