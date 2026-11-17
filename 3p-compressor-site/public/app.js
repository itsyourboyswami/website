const form = document.querySelector('#compress-form');
const picker = document.querySelector('#file-input');
const drop = document.querySelector('.dropzone');
const submit = document.querySelector('.submit-compress');
const status = document.querySelector('.upload-state');
const sizeLimit = 100 * 1024 * 1024;
const browse = document.querySelector('.browse-button');
let selected = null;
const isPackedFile = file => file?.name.toLowerCase().endsWith('.3p');

function choose(file) {
  if (!file) return;
  if (file.size > sizeLimit) { status.textContent = 'That file is over the 100 MB limit.'; status.className = 'upload-state error'; return; }
  selected = file; submit.disabled = false; submit.textContent = 'Click me';
  const action = isPackedFile(file) ? 'will be De-Compressed' : 'will be compressed';
  status.textContent = `${file.name} · ${(file.size / 1048576).toFixed(2)} MB · ${action}`; status.className = 'upload-state';
}
browse.addEventListener('click', () => picker.click());
picker.addEventListener('change', () => choose(picker.files[0]));
for (const ev of ['dragenter', 'dragover']) drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.add('is-over'); });
for (const ev of ['dragleave', 'drop']) drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.remove('is-over'); });
drop.addEventListener('drop', e => choose(e.dataTransfer.files[0]));
form.addEventListener('submit', async e => {
  e.preventDefault(); if (!selected) return;
  const decompress = isPackedFile(selected); const action = decompress ? 'De-Compressing' : 'Compressing';
  submit.disabled = true; submit.textContent = 'Working…'; status.textContent = `Uploading and ${action.toLowerCase()} securely on the server. This may take a moment.`;
  const body = new FormData(); body.append('file', selected); const jobId = crypto.randomUUID(); let polling = true; let pollBusy = false;
  const poll = async () => {
    if (!polling || pollBusy) return; pollBusy = true;
    try {
      const r = await fetch(`/api/compress-status/${jobId}`); if (!r.ok) return; const job = await r.json();
      if (job.state === 'uploading') status.textContent = 'Uploading file to the compressor…';
      else if (job.state === 'compressing' || job.state === 'decompressing') status.textContent = `${job.state === 'decompressing' ? 'De-Compressing' : 'Compressing'} on the server… ${Math.floor(job.progress || 0)}%`;
      else if (job.state === 'ready') status.textContent = 'Processing complete. Preparing your download…';
    } catch {} finally { pollBusy = false; }
  };
  const pollTimer = setInterval(poll, 1000); poll();
  try {
    const response = await fetch('/api/compress', { method: 'POST', headers: { 'X-Job-ID': jobId }, body });
    if (!response.ok) { const data = await response.json(); throw new Error(data.error || 'Compression failed.'); }
    const blob = await response.blob();
    const disposition = response.headers.get('content-disposition') || '';
    const encodedName = disposition.match(/filename\*=UTF-8''([^;]+)/i)?.[1];
    const quotedName = disposition.match(/filename="([^"]+)"/i)?.[1];
    const fallbackName = decompress ? selected.name.replace(/\.3p$/i, '') : `${selected.name}.3p`;
    let downloadName = fallbackName; try { if (encodedName) downloadName = decodeURIComponent(encodedName); else if (quotedName) downloadName = quotedName; } catch {}
    const url = URL.createObjectURL(blob); const a = document.createElement('a'); a.href = url; a.download = downloadName; document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
    status.textContent = decompress ? 'Done — your file has been De-Compressed and downloaded.' : 'Done — your compressed .3p file has been downloaded.'; status.className = 'upload-state success';
  } catch (err) { status.textContent = err.message; status.className = 'upload-state error'; }
  finally { polling = false; clearInterval(pollTimer); submit.disabled = false; submit.textContent = 'Click me'; selected = null; picker.value = ''; }
});

document.querySelector('#contact-form').addEventListener('submit', async e => {
  e.preventDefault(); const form = e.currentTarget; const button = form.querySelector('button[type=submit]'); const message = form.querySelector('.contact-status');
  button.disabled = true; message.textContent = 'Sending…';
  try {
    const values = Object.fromEntries(new FormData(form)); const response = await fetch('/api/contact', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(values) }); const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Could not send your message.');
    form.reset(); message.textContent = 'Thanks — your message is on its way.'; message.className = 'contact-status success';
  } catch (err) { message.textContent = err.message; message.className = 'contact-status error'; }
  finally { button.disabled = false; }
});

const mobile = matchMedia('(max-width: 760px), (hover: none) and (pointer: coarse)');
function updateDownloads() {
  document.body.classList.toggle('mobile-browser', mobile.matches);
  for (const link of document.querySelectorAll('[data-installer]')) {
    fetch(link.getAttribute('href'), { method: 'HEAD' }).then(r => { if (!r.ok) { link.removeAttribute('download'); link.classList.add('unavailable'); link.setAttribute('aria-disabled', 'true'); link.title = 'Installer will be available when a release binary is added.'; link.innerHTML = 'Release coming soon <span>↗</span>'; } }).catch(() => {});
  }
}
updateDownloads(); mobile.addEventListener?.('change', updateDownloads);
document.querySelector('.menu-toggle').addEventListener('click', () => document.querySelector('.topbar').classList.toggle('menu-open'));
