const express = require('express');
const multer = require('multer');
const nodemailer = require('nodemailer');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');

// Load the local .env file without replacing environment variables supplied by the host.
const envPath = path.join(__dirname, '.env');
if (fs.existsSync(envPath)) {
  for (const rawLine of fs.readFileSync(envPath, 'utf8').split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const match = line.match(/^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$/);
    if (!match || process.env[match[1]] !== undefined) continue;
    let value = match[2].trim();
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) value = value.slice(1, -1);
    else value = value.replace(/\s+#.*$/, '').trim();
    process.env[match[1]] = value;
  }
}

const app = express();
const root = __dirname;
const uploadDir = path.join(os.tmpdir(), '3p-site-uploads');
fs.mkdirSync(uploadDir, { recursive: true });
const maxBytes = Number(process.env.MAX_UPLOAD_MB || 100) * 1024 * 1024;
const upload = multer({ dest: uploadDir, limits: { fileSize: maxBytes, files: 1 } });
let activeJobs = 0;
const jobs = new Map();
app.use(express.json({ limit: '16kb' }));
app.use(express.static(path.join(root, 'public')));
app.use('/downloads', express.static(path.join(root, 'downloads'), { fallthrough: true, dotfiles: 'deny' }));

function runCli(args, onProgress = () => {}) {
  return new Promise((resolve, reject) => {
    const exe = process.env.P3CLI || path.join(root, process.platform === 'win32' ? 'p3cli.exe' : 'p3cli');
    const child = spawn(exe, args, { windowsHide: true });
    let output = '';
    let progressTail = '';
    const collect = d => {
      const chunk = d.toString(); output = (output + chunk).slice(-12000);
      progressTail = (progressTail + chunk).slice(-500);
      const matches = [...progressTail.matchAll(/(\d+(?:\.\d+)?)%\s+out\s+(\d+)/g)];
      if (matches.length) onProgress(Math.max(0, Math.min(100, Number(matches[matches.length - 1][1]))));
    };
    child.stdout.on('data', collect);
    child.stderr.on('data', collect);
    child.on('error', reject);
    child.on('close', code => code === 0 ? resolve(output) : reject(new Error(output || `Compressor exited with ${code}`)));
  });
}

app.get('/api/compress-status/:id', (req, res) => {
  const id = req.params.id;
  if (!/^[0-9a-f-]{36}$/i.test(id)) return res.status(400).json({ error: 'Invalid job ID.' });
  const job = jobs.get(id);
  if (!job) return res.status(404).json({ error: 'Compression job expired or was not found.' });
  res.json(job);
});

app.post('/api/compress', (req, res, next) => {
  const requestedId = req.get('X-Job-ID');
  req.compressionJobId = /^[0-9a-f-]{36}$/i.test(requestedId || '') ? requestedId : crypto.randomUUID();
  jobs.set(req.compressionJobId, { state: 'uploading', progress: 0 });
  setTimeout(() => jobs.delete(req.compressionJobId), 10 * 60 * 1000).unref?.();
  next();
}, upload.single('file'), async (req, res) => {
  const jobId = req.compressionJobId;
  if (!req.file) return res.status(400).json({ error: 'Choose a file to compress.' });
  if (activeJobs >= 2) { jobs.set(jobId, { state: 'error', progress: 0, error: 'The online compressor is busy. Please try again shortly.' }); fs.rm(req.file.path, { force: true }, () => {}); return res.status(429).json({ error: 'The online compressor is busy. Please try again shortly.' }); }
  activeJobs++;
  const input = req.file.path;
  const safeName = path.basename(req.file.originalname || 'file').replace(/[\r\n]/g, '_');
  const decompress = path.extname(safeName).toLowerCase() === '.3p';
  const action = decompress ? 'decompressing' : 'compressing';
  const workDir = fs.mkdtempSync(path.join(uploadDir, `${jobId}-`));
  try {
    jobs.set(jobId, { state: action, progress: 0 });
    const mode = decompress ? 'x' : 'c';
    await runCli([mode, input, workDir, '-t', String(Math.max(1, Math.min(os.cpus().length, 2)))], progress => jobs.set(jobId, { state: action, progress }));
    const outputs = fs.readdirSync(workDir).filter(name => name !== '.' && name !== '..');
    if (outputs.length !== 1) throw new Error('Compressor did not create exactly one output file.');
    const outputPath = path.join(workDir, outputs[0]);
    const downloadName = decompress ? outputs[0] : `${safeName}.3p`;
    jobs.set(jobId, { state: 'ready', action, progress: 100 });
    res.download(outputPath, downloadName, err => { activeJobs--; fs.rm(input, { force: true }, () => {}); fs.rm(workDir, { recursive: true, force: true }, () => {}); if (err && !res.headersSent) res.status(500).end(); });
  } catch (error) {
    activeJobs--; fs.rm(input, { force: true }, () => {}); fs.rm(workDir, { recursive: true, force: true }, () => {});
    console.error('Compression failed:', error.message);
    jobs.set(jobId, { state: 'error', progress: 0, error: error.message });
    if (error.code === 'ENOENT') return res.status(503).json({ error: 'The compressor program is not installed on this server yet. On Windows, run build-p3cli.bat in the website folder, then try again.' });
    if (String(error.message).includes('3221225785') || String(error.message).toLowerCase().includes('c0000139')) return res.status(503).json({ error: 'Windows could not load the compressor executable (0xC0000139). Rebuild it with build-p3cli-msvc.bat from the x64 Native Tools Command Prompt, then try again.' });
    const engineMessage = error.message.match(/error\(\d+\):\s*([^\r\n]+)/i)?.[1];
    res.status(500).json({ error: engineMessage ? `${decompress ? 'Decompression' : 'Compression'} failed: ${engineMessage}` : 'File processing failed. Check the server log for details.' });
  }
});

app.post('/api/contact', async (req, res) => {
  const { name, email, phone, message } = req.body || {};
  if (!name || !email || !message || !/^\S+@\S+\.\S+$/.test(email) || String(message).length > 5000)
    return res.status(400).json({ error: 'Please provide your name, a valid email, and a message under 5,000 characters.' });
  if (!process.env.SMTP_HOST || !process.env.SMTP_USER || !process.env.SMTP_PASS)
    return res.status(503).json({ error: 'Contact delivery is not configured yet. Please email thisispankajkc@gmail.com directly.' });
  try {
    const transport = nodemailer.createTransport({ host: process.env.SMTP_HOST, port: Number(process.env.SMTP_PORT || 587), secure: process.env.SMTP_SECURE === 'true', auth: { user: process.env.SMTP_USER, pass: process.env.SMTP_PASS } });
    await transport.sendMail({ from: process.env.SMTP_FROM || process.env.SMTP_USER, to: process.env.CONTACT_TO || 'thisispankajkc@gmail.com', replyTo: email, subject: `3P Compressor website inquiry from ${String(name).slice(0, 100)}`, text: `Name: ${name}\nEmail: ${email}\nPhone: ${phone || 'Not provided'}\n\n${message}` });
    res.json({ ok: true });
  } catch (error) { console.error('Contact email failed:', error.message); res.status(502).json({ error: 'Message could not be delivered right now. Please try again later.' }); }
});

app.get('/api/health', (_req, res) => res.json({ ok: true }));
app.use((err, _req, res, _next) => { if (err instanceof multer.MulterError) return res.status(err.code === 'LIMIT_FILE_SIZE' ? 413 : 400).json({ error: err.code === 'LIMIT_FILE_SIZE' ? `Maximum file size is ${process.env.MAX_UPLOAD_MB || 100} MB.` : 'Upload could not be accepted.' }); console.error(err); res.status(500).json({ error: 'Unexpected server error.' }); });
const port = Number(process.env.PORT || 3000);
app.listen(port, () => console.log(`3P Compressor site listening on port ${port}`));
