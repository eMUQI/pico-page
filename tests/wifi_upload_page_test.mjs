// Executes the shipped browser script with a small DOM and transport stand-in.
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=readFileSync(new URL('../firmware/components/platform_read_pico/upload_page.h',import.meta.url),'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const ids=['route','keep','drop-label','drop','files','list','stop','again','pick','result','counts','count','tally','result-title'];
const elements=Object.fromEntries(ids.map(id=>[id,{hidden:false,textContent:'',innerHTML:''}]));
let handler=()=>200,requests=[];
class Request {
 constructor(){this.upload={};this.headers={};}
 open(method,url){assert.equal(method,'POST');assert.ok(['/check','/upload'].includes(url));this.url=url;}
 setRequestHeader(k,v){this.headers[k]=v;}
 abort(){}
 send(body){
  requests.push(this);
  const result=handler(this);
  if(result==='error')return this.onerror();
  if(result==='abort')return this.onabort();
  if(body&&this.upload.onprogress)this.upload.onprogress({lengthComputable:true,loaded:body.size,total:body.size});
  this.status=result;this.responseText=result===200?'已保存':'拒绝 '+result;this.onload();
 }
}
const context=vm.createContext({document:{getElementById:id=>elements[id]},XMLHttpRequest:Request,Uint8Array,
 token:'test',route:{hotspot:false,network:'home<"x">'}});
vm.runInContext(script,context);
const book=(name,text='123456789')=>{const file=new Blob([text]);file.name=name;return file;};
const choose=async(...files)=>{elements.files.files=files;await elements.files.onchange();};
const list=()=>elements.list.innerHTML;

// The route line escapes the network name and the footer matches the route.
assert.equal(elements.route.innerHTML,'经已保存的 Wi-Fi“home&#60;&#34;x&#34;&#62;”');
assert.match(elements.keep.textContent,/^请保持本页打开。/);

// One request pair per book with the CRC, size and encoded name.
await choose(book('测试.epub'));
assert.deepEqual(requests.map(r=>r.url),['/check','/upload']);
assert.equal(requests[0].headers['X-Pico-Size'],'9');
assert.equal(requests[0].headers['X-Pico-CRC'],'cbf43926');
assert.equal(requests[0].headers['X-Pico-Name'],encodeURIComponent('测试.epub'));
assert.equal(requests[0].headers['X-Pico-Token'],'test');
assert.equal(elements['result-title'].textContent,'已保存 1 本');
assert.ok(list().includes('测试.epub')&&list().includes('class="tag">已保存'));
assert.equal(elements.pick.hidden,true);assert.equal(elements.again.hidden,false);assert.equal(elements.stop.hidden,true);

// A refusal of one book skips it; local checks never reach the device.
requests=[];
handler=r=>r.headers['X-Pico-Name']===encodeURIComponent('边城.epub')?409:200;
await choose(book('呐喊.epub'),book('边城.epub'),book('bad.pdf'),book('empty.txt',''),book('骆驼祥子.txt'));
assert.deepEqual(requests.map(r=>r.url),['/check','/upload','/check','/check','/upload']);
assert.equal(elements['result-title'].textContent,'已保存 2 本，3 本没有完成');
assert.ok(list().includes('拒绝 409')&&list().includes('仅支持 EPUB 或 TXT 文件。')&&list().includes('1 字节至 64 MiB'));
assert.ok(list().includes('呐喊.epub')&&list().includes('骆驼祥子.txt')&&!list().includes('data-retry'));
assert.equal((list().match(/class="tag">已保存/g)||[]).length,2);

// A lost connection during upload leaves the result unconfirmed, offers a retry and stops the batch.
requests=[];
handler=r=>r.url==='/upload'&&r.headers['X-Pico-Name']===encodeURIComponent('b.epub')?'error':200;
await choose(book('a.epub'),book('b.epub'),book('c.epub'));
assert.deepEqual(requests.map(r=>r.url),['/check','/upload','/check','/upload']);
assert.ok(list().includes('可能已经保存，请先在书库核对')&&list().includes('data-retry="1"'));
assert.ok(list().includes('整批已停止，未上传。')&&list().includes('data-retry="2"'));
assert.equal(elements['result-title'].textContent,'已保存 1 本，2 本没有完成');
// Retrying runs that one book again; a copy that did arrive is reported as a same-name refusal.
requests=[];handler=r=>r.url==='/check'?409:200;
await elements.list.onclick({target:{dataset:{retry:'1'}}});
assert.deepEqual(requests.map(r=>r.url),['/check']);
assert.ok(list().includes('拒绝 409')&&list().includes('data-retry="2"'));

// A refusal that is not about one book stops the batch.
requests=[];handler=()=>403;
await choose(book('x.epub'),book('y.epub'));
assert.equal(requests.length,1);
assert.ok(list().includes('拒绝 403')&&list().includes('整批已停止'));

// Stopping aborts the current transfer and marks the rest.
requests=[];
handler=r=>{if(r.url==='/upload'){elements.stop.onclick();return 'abort';}return 200;};
await choose(book('p.epub'),book('q.epub'));
assert.ok(list().includes('结果未确认')&&list().includes('已停止，未上传。'));
assert.equal(requests.length,2);
console.log('Wi-Fi page: per-book rows, refusals, unconfirmed retry, batch stop and escaping passed');
