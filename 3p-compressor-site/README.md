# 3P Compressor website

Deployable Node.js website for the portable C++17 3P compression engine. The web page includes a temporary online upload service, desktop release links, pricing, and a contact form.

## Run locally

Requirements: Node.js 20+ and a C++17 compiler (`g++` on Linux/macOS; MinGW-w64/MSYS2 `g++` on Windows).

```sh
npm install
g++ -std=c++17 -O3 -pthread p3cli.cpp -o p3cli
cp .env.example .env
npm start
```

On Windows, build the server-side compressor before starting the site. The recommended build uses MSVC: open **x64 Native Tools Command Prompt for Visual Studio/Build Tools**, change directory to the website folder, then run `build-p3cli-msvc.bat`. This creates `p3cli.exe` beside `server.js`, where the upload endpoint expects it. MinGW users can run `build-p3cli.bat` or `npm run build:compressor:windows`. If using another location, set `P3CLI` to the full executable path.

Open `http://localhost:3000`. The same online tool compresses regular files to `.3p` and decompresses `.3p` files back to their stored original name. Online jobs are limited to 100 MB by default, and at most two run concurrently. The upload page displays progress reported by the CLI. Change `MAX_UPLOAD_MB` to suit the server’s memory and storage limits. The service removes temporary uploads and output after sending the download.

## Configure contact email

Contact submissions are sent directly to `CONTACT_TO` (defaults to `thisispankajkc@gmail.com`) and use the visitor’s email as `Reply-To`, so replying to the notice reaches the person who contacted you. No separate Gmail forwarding rule is needed. The public-facing address and `mailto:` links are also in `public/index.html`.

For Gmail SMTP, turn on 2-Step Verification on the sending Google Account, then create an App Password. Gmail SMTP uses `smtp.gmail.com` with STARTTLS on port `587`; Google requires authentication. Put these values in `.env`:

```dotenv
CONTACT_TO=thisispankajkc@gmail.com
SMTP_HOST=smtp.gmail.com
SMTP_PORT=587
SMTP_SECURE=false
SMTP_USER=thisispankajkc@gmail.com
SMTP_PASS=your-16-character-app-password
SMTP_FROM=thisispankajkc@gmail.com
```

Use the 16-character App Password as `SMTP_PASS`, not the account’s regular password. Keep `.env` private; it is ignored by Git. On a hosting provider, enter these as server-side environment variables instead of committing the `.env` file. The server loads a local `.env` automatically and does not overwrite variables already set by the host.

For Amazon SES standard outbound SMTP, use the SES SMTP endpoint and SMTP credentials for the same AWS Region:

```dotenv
CONTACT_TO=thisispankajkc@gmail.com
SMTP_HOST=email-smtp.<region>.amazonaws.com
SMTP_PORT=587
SMTP_SECURE=false
SMTP_USER=<SES SMTP username>
SMTP_PASS=<SES SMTP password>
SMTP_FROM=<verified sender identity>
```

SES SMTP credentials are separate from AWS access keys. Verify the sender identity in SES; while SES is in its sandbox, verify the recipient too or request production access. A host ending in `mail-manager-smtp.amazonaws.com` is a Mail Manager ingress endpoint; use it only when its traffic policy and rule set route the message to the intended recipient. The app needs `SMTP_HOST`, `SMTP_USER`, and `SMTP_PASS` present in the running Node process or it returns “Contact delivery is not configured yet.”

Never commit `.env` or SMTP credentials. The form returns a configuration error until SMTP is set up; it does not pretend that a message was delivered.

## Quick deploy on Amazon Linux 2023

The ZIP extracts into a nested `3p-compressor-site` directory. See [EC2-QUICKSTART.md](EC2-QUICKSTART.md) for the short setup commands, Linux compressor build, private `.env`, and systemd service restart.

## Add desktop releases

Copy release binaries into `downloads/` with these exact names:

* `3P-Compressor-Setup.exe`
* `3P-Compressor-Linux.AppImage`
* `3P-Compressor-macOS.dmg`

The supplied Windows GUI build sources and resources are preserved in `desktop-source/`; the server uses the root `engine3p.hpp` and `p3cli.cpp` to build the online compressor. The source bundle does not include release installers for any platform. The Linux/macOS GUI builds and signed installers still need to be produced. Until a binary exists, the page labels that platform’s link “Release coming soon.” Mobile browsers see the online compressor and a note to visit on a desktop; installer cards are hidden.

## Deploy with Docker

```sh
docker build -t 3p-compressor-site .
docker run --rm -p 3000:3000 --env-file .env 3p-compressor-site
```

Provide a persistent volume for `downloads/` if release assets are managed outside the image. Put the service behind HTTPS and a reverse proxy that supports request bodies at least as large as `MAX_UPLOAD_MB`. The online compressor uses temporary disk space for both input and output, so provision adequate ephemeral storage and monitor memory/CPU before increasing concurrent jobs or upload size.
