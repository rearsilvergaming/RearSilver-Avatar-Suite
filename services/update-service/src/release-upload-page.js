export const releaseUploadPage = `<!doctype html>
<html lang="en-GB">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RearSilver Avatar Suite release publisher</title>
<style>
:root{color-scheme:dark;--bg:#0b0f14;--panel:#121923;--field:#1b2430;--border:#354255;--text:#e8ebef;--muted:#9eafc4;--cyan:#00d4ff;--gold:#ffb800;--green:#61d095}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px system-ui,Segoe UI,sans-serif}
.shell{max-width:820px;margin:0 auto;padding:32px 20px}
h1{font-size:27px;margin:0 0 8px}
.intro,.help,.status{color:var(--muted);line-height:1.55}
.panel{margin-top:22px;padding:22px;background:var(--panel);border:1px solid var(--border);border-radius:14px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:15px}
.field{display:flex;flex-direction:column;gap:7px}
.wide{grid-column:1/-1}
.check{align-items:center;display:flex;gap:9px;font-weight:700}
.check input{width:auto}
label{font-weight:700}
input,select,textarea,button{width:100%;padding:11px 12px;border:1px solid var(--border);border-radius:9px;background:var(--field);color:var(--text);font:inherit}
textarea{min-height:125px;resize:vertical}
button{cursor:pointer;background:#075166;border-color:var(--cyan);font-weight:750}
button.publish{background:#17683f;border-color:var(--green)}
button:disabled{cursor:not-allowed;opacity:.5}
progress{width:100%;height:18px;margin-top:16px;accent-color:var(--cyan)}
.warning{border-left:3px solid var(--gold);padding-left:12px}
.result,pre{white-space:pre-wrap;word-break:break-word}
pre{background:#0c131c;border:1px solid var(--border);border-radius:9px;color:#cde1f5;max-height:360px;overflow:auto;padding:13px}
.hidden{display:none}
@media(max-width:620px){.grid{grid-template-columns:1fr}.wide{grid-column:auto}}
</style>
</head>
<body>
<main class="shell">
<h1>Release installer and manifest</h1>
<p class="intro">Upload a versioned installer, review the generated update manifest, then publish it to the selected channel.</p>

<section class="panel">
<div class="grid">
<div class="field wide">
<label for="token">Release upload token</label>
<input id="token" type="password" autocomplete="off">
<span class="help">This stays in this page's memory and is sent only to the Avatar Suite update Worker.</span>
</div>

<div class="field">
<label for="channel">Channel</label>
<select id="channel">
<option value="owner-build">Owner Build</option>
<option value="private-beta">Private Beta</option>
</select>
</div>

<div class="field">
<label for="version">Version</label>
<input id="version">
<span class="help" id="versionHelp"></span>
</div>

<div class="field">
<label for="minimumVersion">Minimum supported version</label>
<input id="minimumVersion">
<span class="help" id="minimumHelp"></span>
</div>

<div class="field">
<label>Update requirement</label>
<label class="check">
<input id="mandatory" type="checkbox">
Mandatory update
</label>
</div>

<div class="field wide">
<label for="releaseNotes">Patch notes</label>
<textarea id="releaseNotes" placeholder="One short patch note per line, up to six lines."></textarea>
</div>

<div class="field wide">
<label for="releaseNotesUrl">Full release-notes URL (optional)</label>
<input id="releaseNotesUrl" type="url" placeholder="Add this once the Avatar Suite website is available">
</div>

<div class="field wide">
<label for="installer">Installer</label>
<input id="installer" type="file" accept=".exe,application/vnd.microsoft.portable-executable">
</div>

<div class="field wide">
<button id="upload">Upload and prepare manifest</button>
</div>
</div>

<progress id="progress" max="100" value="0"></progress>
<p class="status result" id="status">Complete the release details and choose the installer.</p>
</section>

<section class="panel hidden" id="review">
<h2>Manifest review</h2>
<p class="help">Review this carefully. Publishing makes the update visible to installed builds on this channel. After publication, changing the installer, notes, URL, requirement, or minimum version requires a new version number.</p>
<pre id="manifestPreview"></pre>
<button class="publish" id="publish">Publish update</button>
</section>

<p class="help warning">Uploading does not publish automatically. The live channel changes only after you select Publish update.</p>
</main>

<script>
(()=>{
const token=document.getElementById('token');
const channel=document.getElementById('channel');
const version=document.getElementById('version');
const minimumVersion=document.getElementById('minimumVersion');
const mandatory=document.getElementById('mandatory');
const notes=document.getElementById('releaseNotes');
const notesUrl=document.getElementById('releaseNotesUrl');
const file=document.getElementById('installer');
const uploadButton=document.getElementById('upload');
const publishButton=document.getElementById('publish');
const progress=document.getElementById('progress');
const status=document.getElementById('status');
const review=document.getElementById('review');
const preview=document.getElementById('manifestPreview');
const versionHelp=document.getElementById('versionHelp');
const minimumHelp=document.getElementById('minimumHelp');

const uploadApi='/v1/admin/releases/upload';
const publishApi='/v1/admin/releases/publish';

const staticExamples={
  'owner-build':{
    version:'1.0.0-owner.5',
    minimum:'1.0.0-owner.5'
  },
  'private-beta':{
    version:'1.0.0-beta.2',
    minimum:'1.0.0-beta.2'
  }
};

let draft=null;
let knownLiveVersion='';

const headers=()=>({
  authorization:'Bearer '+token.value
});

const request=async(url,options={})=>{
  const response=await fetch(url,options);
  const text=await response.text();
  let body={};

  try{
    body=JSON.parse(text);
  }catch{
    body={error:text};
  }

  if(!response.ok){
    throw new Error(
      body?.error||
      ('Request failed ('+response.status+')')
    );
  }

  return body;
};

const parseVersion=(value,selectedChannel)=>{
  const match=String(value||'').match(
    /^(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)-(owner|beta)\\.(0|[1-9]\\d*)$/
  );

  const expectedFamily=
    selectedChannel==='owner-build'
      ?'owner'
      :'beta';

  if(!match||match[4]!==expectedFamily){
    return null;
  }

  return{
    raw:value,
    major:BigInt(match[1]),
    minor:BigInt(match[2]),
    patch:BigInt(match[3]),
    prerelease:BigInt(match[5]),
    family:match[4]
  };
};

const compareVersions=(left,right,selectedChannel)=>{
  const a=parseVersion(left,selectedChannel);
  const b=parseVersion(right,selectedChannel);

  if(!a||!b){
    return null;
  }

  for(const field of ['major','minor','patch','prerelease']){
    if(a[field]<b[field])return-1;
    if(a[field]>b[field])return 1;
  }

  return 0;
};

const incrementVersion=(value,selectedChannel)=>{
  const parsed=parseVersion(value,selectedChannel);

  if(!parsed){
    return null;
  }

  return (
    parsed.major+'.'+
    parsed.minor+'.'+
    parsed.patch+'-'+
    parsed.family+'.'+
    (parsed.prerelease+1n)
  );
};

const expectedFilename=(selectedChannel,selectedVersion)=>
  selectedChannel==='owner-build'
    ?'RearSilver-Avatar-Suite-Owner-'+selectedVersion+'-Setup.exe'
    :'RearSilver-Avatar-Suite-Private-Beta-'+selectedVersion+'-Setup.exe';

const validHttpsUrl=value=>{
  if(value===''){
    return true;
  }

  try{
    return new URL(value).protocol==='https:';
  }catch{
    return false;
  }
};

const validSha256=value=>
  /^[0-9a-f]{64}$/.test(value);

const patchNotes=()=>
  notes.value
    .split(/\\r?\\n/)
    .map(note=>note.trim())
    .filter(Boolean);

const hex=buffer=>
  Array.from(
    new Uint8Array(buffer),
    byte=>byte.toString(16).padStart(2,'0')
  ).join('');

const setBusy=busy=>{
  token.disabled=busy;
  channel.disabled=busy;
  version.disabled=busy;
  minimumVersion.disabled=busy;
  mandatory.disabled=busy;
  notes.disabled=busy;
  notesUrl.disabled=busy;
  file.disabled=busy;
  uploadButton.disabled=busy;
};

const params=(release,extra={})=>{
  const value=new URLSearchParams({
    channel:release.channel,
    version:release.version,
    filename:release.filename,
    sha256:release.sha256,
    size:String(release.size),
    ...extra
  });

  return uploadApi+'?'+value;
};

const updateGuidance=async()=>{
  const selectedChannel=channel.value;
  const examples=staticExamples[selectedChannel];

  knownLiveVersion='';
  version.placeholder=examples.version;
  minimumVersion.value=examples.minimum;
  minimumVersion.placeholder=examples.minimum;
  versionHelp.textContent='Expected format: '+examples.version;
  minimumHelp.textContent='Enter exactly: '+examples.minimum;

  try{
    const response=await fetch(
      '/v1/updates/'+selectedChannel+'/windows-x64',
      {cache:'no-store'}
    );

    if(!response.ok){
      return;
    }

    const manifest=await response.json();
    const next=incrementVersion(
      manifest.version,
      selectedChannel
    );

    if(!next){
      return;
    }

    knownLiveVersion=manifest.version;
    version.placeholder=next;
    versionHelp.textContent='Suggested next version: '+next;
  }catch{
  }
};

const validateInput=selected=>{
  const selectedChannel=channel.value;
  const selectedVersion=version.value.trim();
  const selectedMinimum=minimumVersion.value.trim();
  const releaseNotes=patchNotes();
  const releaseNotesUrl=notesUrl.value.trim();

  if(!token.value){
    return'Release upload token is required.';
  }

  if(!parseVersion(selectedVersion,selectedChannel)){
    return'Enter a valid channel version using the format shown.';
  }

  if(
    !parseVersion(
      selectedMinimum,
      selectedChannel
    )
  ){
    return'Enter a valid minimum supported version for this channel.';
  }

  if(
    compareVersions(
      selectedMinimum,
      selectedVersion,
      selectedChannel
    )>0
  ){
    return'The minimum supported version cannot be newer than the release.';
  }

  if(
    knownLiveVersion &&
    selectedVersion===knownLiveVersion
  ){
    return'This version is already published. Use a new version.';
  }

  if(!selected){
    return'Choose the installer first.';
  }

  if(
    selected.name!==
    expectedFilename(
      selectedChannel,
      selectedVersion
    )
  ){
    return'Installer filename does not match the selected channel and version.';
  }

  if(
    selected.size<1||
    selected.size>5*1024*1024*1024
  ){
    return'Installer size is invalid.';
  }

  if(
    releaseNotes.length>6||
    releaseNotes.some(
      note=>
        note.length<1||
        note.length>180
    )
  ){
    return'Use no more than six patch notes, with no more than 180 characters per line.';
  }

  if(!validHttpsUrl(releaseNotesUrl)){
    return'The full release-notes URL must be blank or use HTTPS.';
  }

  return'';
};

const showDraft=release=>{
  draft={
    channel:release.channel,
    version:release.version,
    minimum_supported_version:
      release.minimum_supported_version,
    mandatory:release.mandatory,
    filename:release.filename,
    size:release.size,
    sha256:release.sha256,
    release_notes:release.release_notes,
    release_notes_url:
      release.release_notes_url
  };

  const display={
    schema:1,
    product:'rearsilver-avatar-suite',
    channel:draft.channel,
    platform:'windows-x64',
    version:draft.version,
    minimum_supported_version:
      draft.minimum_supported_version,
    minimum_updater_schema:1,
    mandatory:draft.mandatory,
    published_at:'Generated when published',
    installer:{
      filename:draft.filename,
      size:draft.size,
      sha256:draft.sha256,
      download_request_url:
        'Generated by the Worker'
    },
    release_notes:draft.release_notes,
    release_notes_url:
      draft.release_notes_url
  };

  preview.textContent=JSON.stringify(
    display,
    null,
    2
  );

  review.classList.remove('hidden');
  publishButton.disabled=false;
};

channel.onchange=()=>{
  updateGuidance();
};

uploadButton.onclick=async()=>{
  const selected=file.files[0];
  const validationError=validateInput(selected);

  if(validationError){
    status.textContent=validationError;
    return;
  }

  const release={
    channel:channel.value,
    version:version.value.trim(),
    minimum_supported_version:
      minimumVersion.value.trim(),
    mandatory:mandatory.checked,
    filename:selected.name,
    size:selected.size,
    sha256:'',
    release_notes:patchNotes(),
    release_notes_url:
      notesUrl.value.trim()
  };

  setBusy(true);
  publishButton.disabled=true;
  review.classList.add('hidden');
  progress.value=0;
  draft=null;

  let uploadId='';

  try{
    status.textContent=
      'Calculating installer SHA-256...';

    release.sha256=hex(
      await crypto.subtle.digest(
        'SHA-256',
        await selected.arrayBuffer()
      )
    );

    if(!validSha256(release.sha256)){
      throw new Error(
        'Installer SHA-256 calculation failed'
      );
    }

    status.textContent=
      'Creating protected multipart upload...';

    const created=await request(
      uploadApi,
      {
        method:'POST',
        headers:{
          ...headers(),
          'content-type':'application/json'
        },
        body:JSON.stringify({
          channel:release.channel,
          version:release.version,
          filename:release.filename,
          size:release.size,
          sha256:release.sha256
        })
      }
    );

    uploadId=created.uploadId||'';

    if(!created.alreadyExists){
      const partSize=created.partSize;
      const count=Math.ceil(
        selected.size/partSize
      );

      const totalMb=(
        selected.size/1048576
      ).toFixed(1);

      const parts=[];

      for(let index=0;index<count;index++){
        const number=index+1;
        const start=index*partSize;
        const end=Math.min(
          start+partSize,
          selected.size
        );

        const percent=Math.round(
          number/count*100
        );

        const sentMb=(
          end/1048576
        ).toFixed(1);

        status.textContent=
          'Uploading part '+
          number+
          ' of '+
          count+
          ' ('+
          percent+
          '%, '+
          sentMb+
          ' MB of '+
          totalMb+
          ' MB)...';

        const uploaded=await request(
          params(
            release,
            {
              action:'part',
              uploadId,
              partNumber:String(number)
            }
          ),
          {
            method:'PUT',
            headers:headers(),
            body:selected.slice(start,end)
          }
        );

        parts.push(uploaded);

        progress.value=Math.round(
          number/count*95
        );
      }

      status.textContent=
        'Completing and verifying upload...';

      await request(
        params(
          release,
          {
            action:'complete',
            uploadId
          }
        ),
        {
          method:'POST',
          headers:{
            ...headers(),
            'content-type':'application/json'
          },
          body:JSON.stringify({
            parts,
            size:selected.size,
            sha256:release.sha256
          })
        }
      );
    }

    progress.value=100;

    status.textContent=created.alreadyExists
      ?'Existing installer identity verified. Review the manifest below.'
      :'Upload complete and verified. Review the manifest below.';

    showDraft(release);
  }catch(error){
    status.textContent=
      'Upload failed: '+error.message;

    if(uploadId&&release.sha256){
      try{
        await request(
          params(
            release,
            {
              action:'abort',
              uploadId
            }
          ),
          {
            method:'DELETE',
            headers:headers()
          }
        );
      }catch{
      }
    }
  }finally{
    setBusy(false);
  }
};

publishButton.onclick=async()=>{
  if(!draft){
    return;
  }

  publishButton.disabled=true;

  try{
    status.textContent=
      'Publishing immutable channel manifest...';

    const result=await request(
      publishApi,
      {
        method:'POST',
        headers:{
          ...headers(),
          'content-type':'application/json'
        },
        body:JSON.stringify(draft)
      }
    );

    preview.textContent=JSON.stringify(
      result.manifest,
      null,
      2
    );

    status.textContent=
      'Update published successfully. Installed builds can now discover '+
      result.manifest.version+
      '.';

    knownLiveVersion=
      result.manifest.version;
  }catch(error){
    status.textContent=
      'Publish failed: '+error.message;

    publishButton.disabled=false;
  }
};

updateGuidance();
})();
</script>
</body>
</html>`;
