#pragma once

namespace pico_page::platform {
inline constexpr char upload_page_en[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Pico Page transfer</title>
<style>*{box-sizing:border-box}body{margin:0 auto;max-width:560px;background:#fff;color:#111;font:16px/1.5 system-ui,-apple-system,'PingFang SC','Noto Sans SC',sans-serif}
header{padding:18px 20px;border-bottom:3px solid #111}h1{font-size:22px;margin:0}.sub{font-size:13px;color:#333}.mono{font-family:ui-monospace,Menlo,Consolas,monospace}
.pick{padding:16px 20px;display:flex;flex-direction:column;gap:12px;border-bottom:2px solid #111}
.drop{min-height:64px;border:2px solid #111;display:flex;align-items:center;justify-content:center;font-size:17px;cursor:pointer;text-align:center;padding:12px}
.drop.over{border-width:3px}.drop:focus-visible{outline:3px solid #111;outline-offset:3px}
.hint{margin:0;font-size:13px;color:#333;line-height:1.6}.kinds{display:grid;grid-template-columns:auto 1fr;column-gap:12px}.kinds b{color:#111}.counts{padding:12px 20px 0;display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap;font-size:14px}
.result{padding:20px;display:flex;flex-direction:column;gap:6px;border-bottom:2px solid #111}.result b{font-size:20px}.result span{font-size:14px;line-height:1.6}
.list{padding:4px 20px 0;display:flex;flex-direction:column}
.row{min-height:56px;display:flex;align-items:center;justify-content:space-between;gap:12px;border-bottom:1px solid #cfcfcf;padding:6px 0}
.row>div{display:flex;flex-direction:column;min-width:0;overflow-wrap:anywhere}.row small{font-size:13px;color:#333}.row small.why{color:#111}
.row.wait{color:#333}.tag{font-size:14px;border:2px solid #111;padding:0 6px;white-space:nowrap}
.now{min-height:72px;flex-direction:column;align-items:stretch;justify-content:center;gap:6px;border:2px solid #111;margin:0 -10px;padding:8px 10px}
.now .line{display:flex;flex-direction:row;justify-content:space-between;gap:12px}.now .line b{min-width:0;overflow-wrap:anywhere}.now .line span{font-size:14px;white-space:nowrap}
.bar{height:8px;border:1px solid #111}.bar i{display:block;height:100%;background:#111}
.retry{min-width:72px;min-height:44px;padding:6px;flex-shrink:0;border:2px solid #111;background:#fff;color:#111;font:inherit;font-size:15px;cursor:pointer}
.foot{padding:20px 20px 24px;display:flex;flex-direction:column;gap:10px}
.btn{height:52px;border:2px solid #111;background:#fff;color:#111;font:inherit;font-size:17px;cursor:pointer;width:100%}.primary{background:#111;color:#fff}
[hidden]{display:none!important}</style>
<header><h1>Wi-Fi transfer</h1><div class="sub" id="route"></div></header>
<div class="pick"><label class="drop" id="drop" role="button" tabindex="0"><input id="files" type="file" accept=".epub,.txt,.ttf,.otf,.png,.jpg,.jpeg" multiple hidden><span id="drop-label">Choose books, fonts, or sleep images</span></label>
<div class="hint kinds"><b>Books</b><span>EPUB / TXT · Up to 64 MiB</span><b>Fonts</b><span>TTF / OTF · Up to 32 MiB</span><b>Sleep images</b><span>PNG / JPEG · Up to 2 MiB</span></div>
<p class="hint">Files go into Books, Fonts, or LockScreens. Existing files are not overwritten.</p></div>
<div class="result" id="result" role="status" hidden><b id="result-title"></b><span>Tap “End transfer” on the device to update the library and font list.</span></div>
<div class="counts" id="counts" hidden><span id="count"></span><span id="tally"></span></div>
<div class="list" id="list"></div>
<div class="foot"><button class="btn" id="stop" hidden>Stop upload</button><button class="btn primary" id="again" hidden>Add more files</button><p class="hint" id="keep"></p></div>
<script>
const $=id=>document.getElementById(id),files=$('files'),list=$('list'),stop=$('stop'),again=$('again');
$('drop').onkeydown=e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();if(!e.repeat)files.click();}};
const table=Array.from({length:256},(_,i)=>{let c=i;for(let j=0;j<8;j++)c=(c>>>1)^((c&1)?0xedb88320:0);return c>>>0;});
const esc=s=>String(s).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
const message=(text,values)=>text.replace(/\{(\w+)\}/g,(_,key)=>String(values[key]));
const formatFileSize=n=>n<1048576?Math.max(1,Math.round(n/1024))+' KB':(n/1048576).toFixed(1)+' MB';
const fileRefusalStatuses=new Set([400,409,413,415]);
const kinds=[[/\.(epub|txt)$/i,'Books',64,'Books'],[/\.(ttf|otf)$/i,'Fonts',32,'Fonts'],[/\.(png|jpe?g)$/i,'LockScreens',2,'Sleep images']];
const kind=file=>kinds.find(k=>k[0].test(file.name));
const imageSizeProblem=file=>new Promise(resolve=>{
 if(!globalThis.Image||!globalThis.URL||!URL.createObjectURL)return resolve('');
 const image=new Image(),url=URL.createObjectURL(file),done=why=>{URL.revokeObjectURL(url);resolve(why);};
 image.onload=()=>{const w=image.naturalWidth,h=image.naturalHeight;done(w>4096||h>4096||w*h>8388608?message('Image size is {width} × {height} pixels. The limit is 4096 pixels per side and 8 million pixels in total.',{width:w,height:h}):'');};
 image.onerror=()=>done('');
 image.src=url;
});
let items=[],running=false,stopped=false,xhr=null;
$('route').innerHTML=route.hotspot?'Connected to device hotspot <span class="mono">'+esc(route.network)+'</span>':message('Using saved Wi-Fi “{network}”',{network:esc(route.network)});
$('keep').textContent=route.hotspot?'Keep this page open and stay connected to the hotspot. Closing it interrupts the current file.':'Keep this page open. Closing it interrupts the current file.';
if(globalThis.innerWidth>=520)$('drop-label').textContent='Choose or drop books, fonts, or sleep images';
const row=(item,i)=>{
 const name=esc(item.file.name);
 switch(item.state){
 case 'wait':return '<div class="row wait"><div>'+name+'</div><span>Waiting</span></div>';
 case 'check':case 'upload':case 'verify':{
  const label=item.state==='check'?'Checking':item.state==='verify'?'Saving and verifying':'Uploading · '+item.percent+'%';
  const width=item.state==='check'?0:item.state==='verify'?100:item.percent;
  return '<div class="row now"><div class="line"><b>'+name+'</b><span>'+label+'</span></div><div class="bar"><i style="width:'+width+'%"></i></div></div>';}
 case 'saved':return '<div class="row"><div>'+name+'<small>'+formatFileSize(item.file.size)+' · Saved to '+kind(item.file)[1]+'</small></div><span class="tag">Saved</span></div>';
 case 'rejected':return '<div class="row"><div>'+name+'<small class="why">'+esc(item.why)+'</small></div><span class="tag">Not uploaded</span></div>';
 default:return '<div class="row"><div>'+name+'<small class="why">'+esc(item.why)+'</small></div><button class="retry" data-retry="'+i+'">Retry</button></div>';}
};
const render=()=>{
 const saved=items.filter(i=>i.state==='saved'),rejected=items.filter(i=>i.state==='rejected').length;
 const done=!running&&items.length>0,missing=items.length-saved.length;
 $('result').hidden=!done;$('counts').hidden=done||!items.length;
 $('count').textContent=message('Files: {count}',{count:items.length});
 $('tally').textContent=message('Saved: {saved}',{saved:saved.length})+(rejected?message(' · Not uploaded: {rejected}',{rejected}):'');
 $('result-title').textContent=missing?message('Saved: {saved}. Not completed: {missing}.',{saved:saved.length,missing}):message('Saved: {saved}',{saved:saved.length});
 list.innerHTML=items.map(row).join('');
 stop.hidden=!running;again.hidden=running||!items.length;
};
const checksum=async item=>{
 let crc=0xffffffff;
 for(let offset=0;offset<item.file.size;offset+=65536){
  if(stopped)throw {stop:true};
  const bytes=new Uint8Array(await item.file.slice(offset,offset+65536).arrayBuffer());
  for(const b of bytes)crc=(crc>>>8)^table[(crc^b)&255];
 }
 return ((crc^0xffffffff)>>>0).toString(16);
};
const transmit=(url,item,headers,body,timeoutMs)=>new Promise((resolve,reject)=>{
 xhr=new XMLHttpRequest();xhr.open('POST',url);xhr.timeout=timeoutMs;
 for(const [key,value] of Object.entries(headers))xhr.setRequestHeader(key,value);
 xhr.upload.onprogress=e=>{if(!body||!e.lengthComputable)return;item.percent=Math.floor(e.loaded/e.total*100);item.state=e.loaded===e.total?'verify':'upload';render();};
 xhr.onload=()=>xhr.status===200?resolve():reject({status:xhr.status,why:xhr.responseText||'The device refused this file.'});
 xhr.onerror=xhr.ontimeout=()=>reject({status:0});
 xhr.onabort=()=>reject({status:0,aborted:true});
 xhr.send(body);
});
const sendAndMayContinue=async item=>{
 const k=kind(item.file);
 let local=!k?'Only EPUB, TXT, TTF, OTF, PNG, and JPEG files are supported.':!item.file.size||item.file.size>k[2]*1048576?message('Allowed size for {kind}: 1 byte to {limit} MiB.',{kind:k[3],limit:k[2]}):'';
 if(!local&&k[1]==='LockScreens')local=await imageSizeProblem(item.file);
 if(local){item.state='rejected';item.why=local;return true;}
 item.state='check';item.percent=0;render();
 let sending=false;
 try{
  const headers={'X-Pico-Token':token,'X-Pico-Name':encodeURIComponent(item.file.name),'X-Pico-CRC':await checksum(item),'X-Pico-Size':String(item.file.size)};
  await transmit('/check',item,headers,null,15000);
  if(stopped)throw {stop:true};
  sending=true;item.state='upload';render();
  const refreshAndVerifyBudgetMs=120000,uploadBudgetBytesPerMs=50;
  await transmit('/upload',item,headers,item.file,refreshAndVerifyBudgetMs+item.file.size/uploadBudgetBytesPerMs);
  item.state='saved';return true;
 }catch(e){
  if(e.stop){item.state='stopped';item.why='Stopped. Not uploaded.';return false;}
  if(e.status&&fileRefusalStatuses.has(e.status)){item.state='rejected';item.why=e.status===409&&item.unconfirmed?'A file with this name already exists. It was not uploaded again. It may have been saved before the interruption. Check the device library.':e.why;return true;}
  if(e.status){item.state='failed';item.why=e.why;return false;}
  item.state=sending?'unconfirmed':'failed';
  item.why=sending?'Connection lost. The result is unconfirmed. Tap “Retry”. If a file with this name already exists, check the device library.':e.aborted?'Stopped. Not uploaded.':'Connection lost. Not uploaded. Check your connection to the device.';
  return false;
 }finally{xhr=null;}
};
const pump=async()=>{
 if(running)return;
 running=true;stopped=false;render();
 for(let item;(item=items.find(i=>i.state==='wait'));){
  if(stopped||!await sendAndMayContinue(item)){
   for(const rest of items)if(rest.state==='wait'){rest.state='stopped';rest.why=stopped?'Stopped. Not uploaded.':'The batch was stopped. Not uploaded.';}
   break;
  }
  render();
 }
 running=false;render();
};
const add=chosen=>{if(!chosen.length)return;for(const file of chosen)items.push({file,state:'wait',percent:0});render();return pump();};
files.onchange=()=>{const done=add(files.files);files.value='';return done;};
stop.onclick=()=>{stopped=true;if(xhr)xhr.abort();};
again.onclick=()=>files.click();
list.onclick=e=>{const i=e.target&&e.target.dataset&&e.target.dataset.retry;if(i===undefined)return;const item=items[Number(i)];item.unconfirmed=item.state==='unconfirmed';item.state='wait';render();return pump();};
const drop=$('drop');
document.ondragover=e=>{e.preventDefault();drop.className='drop over';};
document.ondragleave=()=>{drop.className='drop';};
document.ondrop=e=>{e.preventDefault();drop.className='drop';return add(e.dataTransfer.files);};
render();
</script></html>)HTML";
}
