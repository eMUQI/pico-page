#pragma once

namespace pico_page::platform {
// The session token is appended by the server before this script runs.
inline constexpr char upload_page[] = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Pico Page 传书</title>
<style>body{font:18px system-ui;max-width:36em;margin:2em auto;padding:0 1em}button,input{font:inherit;margin:1em 0;padding:.5em}progress{width:100%}</style>
<h1>Wi-Fi 传书</h1><p>选择 EPUB 或 UTF-8 TXT，上传到 Books 文件夹。每个文件最大 64 MiB，同名文件不会被覆盖。</p>
<p>请保持连接设备热点。上传成功后，在设备上选择“结束传书”返回书库。传输期间暂停阅读和同步。</p>
<input id="files" aria-label="选择 EPUB 或 TXT" type="file" accept=".epub,.txt" multiple><br>
<button id="send">上传</button><button id="cancel" disabled>取消</button>
<progress id="progress" max="100" value="0" aria-label="上传进度"></progress><p id="status" role="status"></p>
<script>
const files=document.getElementById('files'),send=document.getElementById('send'),cancel=document.getElementById('cancel'),progress=document.getElementById('progress'),status=document.getElementById('status');
let stopped=false,xhr=null;
const table=Array.from({length:256},(_,i)=>{let c=i;for(let j=0;j<8;j++)c=(c>>>1)^((c&1)?0xedb88320:0);return c>>>0;});
cancel.onclick=()=>{stopped=true;if(xhr)xhr.abort();};
send.onclick=async()=>{
 if(!files.files.length){status.textContent='请先选择 EPUB 或 TXT。';return;}
 send.disabled=files.disabled=true;cancel.disabled=false;stopped=false;
 try{for(const file of files.files){
  if(stopped)throw Error('已取消。已成功上传的文件保留；本次结果请在书库核对。');
  if(!/\.(epub|txt)$/i.test(file.name)||!file.size||file.size>64*1024*1024)throw Error(file.name+'：请选择 1 字节至 64 MiB 的 EPUB 或 UTF-8 TXT。');
  status.textContent='正在校验：'+file.name;progress.value=0;let crc=0xffffffff;
  for(let offset=0;offset<file.size;offset+=65536){
   if(stopped)throw Error('已取消。');
   const bytes=new Uint8Array(await file.slice(offset,offset+65536).arrayBuffer());
   for(const b of bytes)crc=(crc>>>8)^table[(crc^b)&255];
  }
  if(stopped)throw Error('已取消。');
  const headers={'X-Pico-Token':token,'X-Pico-Name':encodeURIComponent(file.name),'X-Pico-CRC':((crc^0xffffffff)>>>0).toString(16),'X-Pico-Size':String(file.size)};
  const transmit=(url,body,timeout)=>new Promise((resolve,reject)=>{
   xhr=new XMLHttpRequest();xhr.open('POST',url);xhr.timeout=timeout;
   for(const [key,value] of Object.entries(headers))xhr.setRequestHeader(key,value);
   xhr.upload.onprogress=e=>{if(!body)return;if(e.lengthComputable)progress.value=e.loaded/e.total*100;if(e.loaded===e.total)status.textContent='正在保存并回读校验：'+file.name;};
   xhr.onload=()=>xhr.status===200?resolve():reject(Error(xhr.responseText||'上传失败，请检查存储卡。'));
   xhr.onerror=xhr.ontimeout=()=>reject(Error('连接中断或超时，上传结果未确认，请在书库核对后重试。'));
   xhr.onabort=()=>reject(Error('已取消，上传结果未确认，请在书库核对。'));
   xhr.send(body);
  });
  status.textContent='正在准备上传：'+file.name;
  await transmit('/check',null,15000);
  if(stopped)throw Error('已取消。');
  status.textContent='正在上传：'+file.name;
  await transmit('/upload',file,300000);
  status.textContent='已保存：'+file.name;
 }
 status.textContent='所选书籍已全部保存。请在设备上结束传书。';
 }catch(e){status.textContent=e.message;}finally{xhr=null;send.disabled=files.disabled=false;cancel.disabled=true;}
};
</script></html>)HTML";
}
