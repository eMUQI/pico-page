import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=readFileSync(new URL('../firmware/components/platform_read_pico/upload_page.h',import.meta.url),'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const traditionalHtml=readFileSync(new URL('../firmware/components/platform_read_pico/upload_page_hant.h',import.meta.url),'utf8');
const traditionalScript=traditionalHtml.match(/<script>([\s\S]*?)<\/script>/)[1];
const englishHtml=readFileSync(new URL('../firmware/components/platform_read_pico/upload_page_en.h',import.meta.url),'utf8');
const englishScript=englishHtml.match(/<script>([\s\S]*?)<\/script>/)[1];
const ids=['route','keep','drop-label','drop','files','list','stop','again','result','counts','count','tally','result-title'];
let elements,context,handler=()=>200,requests=[];
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
const open=(language='zh-Hans')=>{
 elements=Object.fromEntries(ids.map(id=>[id,{hidden:false,textContent:'',innerHTML:''}]));
 context=vm.createContext({document:{getElementById:id=>elements[id]},XMLHttpRequest:Request,Uint8Array,
  token:'test',route:{hotspot:false,network:'home<"x">'}});
 vm.runInContext(language==='en'?englishScript:language==='zh-Hant'?traditionalScript:script,context);requests=[];handler=()=>200;
};
open();
const book=(name,text='123456789')=>{const file=new Blob([text]);file.name=name;return file;};
const choose=async(...files)=>{elements.files.files=files;await elements.files.onchange();};
const list=()=>elements.list.innerHTML;

assert.equal(elements.route.innerHTML,'经已保存的 Wi-Fi「home&#60;&#34;x&#34;&#62;」');
assert.match(elements.keep.textContent,/^请保持本页打开。/);

await choose(book('测试.epub'));
assert.deepEqual(requests.map(r=>r.url),['/check','/upload']);
assert.equal(requests[0].headers['X-Pico-Size'],'9');
assert.equal(requests[0].headers['X-Pico-CRC'],'cbf43926');
assert.equal(requests[0].headers['X-Pico-Name'],encodeURIComponent('测试.epub'));
assert.equal(requests[0].headers['X-Pico-Token'],'test');
assert.equal(requests[0].timeout,15000);assert.equal(requests[1].timeout,120000+9/50);
assert.equal(elements['result-title'].textContent,'已保存 1 个');
assert.ok(list().includes('测试.epub')&&list().includes('class="tag">已保存'));
assert.equal(elements.again.hidden,false);assert.equal(elements.stop.hidden,true);

open();
handler=r=>r.headers['X-Pico-Name']===encodeURIComponent('边城.epub')?409:200;
await choose(book('呐喊.epub'),book('边城.epub'),book('bad.pdf'),book('empty.txt',''),book('骆驼祥子.txt'));
assert.deepEqual(requests.map(r=>r.url),['/check','/upload','/check','/check','/upload']);
assert.equal(elements['result-title'].textContent,'已保存 2 个，3 个没有完成');
assert.ok(list().includes('拒绝 409')&&list().includes('仅支持 EPUB、TXT、TTF、OTF、PNG 和 JPEG 文件。')&&list().includes('书籍大小必须为 1 字节至 64 MiB'));
assert.ok(list().includes('呐喊.epub')&&list().includes('骆驼祥子.txt')&&!list().includes('data-retry'));
assert.equal((list().match(/class="tag">已保存/g)||[]).length,2);

open();
handler=r=>r.url==='/upload'&&r.headers['X-Pico-Name']===encodeURIComponent('b.epub')?'error':200;
await choose(book('a.epub'),book('b.epub'),book('c.epub'));
assert.deepEqual(requests.map(r=>r.url),['/check','/upload','/check','/upload']);
assert.ok(list().includes('结果未确认。可以点「重试」')&&list().includes('data-retry="1"'));
assert.ok(list().includes('整批已停止，未上传。')&&list().includes('data-retry="2"'));
assert.equal(elements['result-title'].textContent,'已保存 1 个，2 个没有完成');
requests=[];handler=r=>r.url==='/check'?409:200;
await elements.list.onclick({target:{dataset:{retry:'1'}}});
assert.deepEqual(requests.map(r=>r.url),['/check']);
assert.ok(list().includes('可能是中断前已保存的这一份')&&list().includes('data-retry="2"'));

open();handler=()=>403;
await choose(book('x.epub'),book('y.epub'));
assert.equal(requests.length,1);
assert.ok(list().includes('拒绝 403')&&list().includes('整批已停止'));

open();
handler=r=>{if(r.url==='/upload'){elements.stop.onclick();return 'abort';}return 200;};
await choose(book('p.epub'),book('q.epub'));
assert.ok(list().includes('结果未确认')&&list().includes('已停止，未上传。'));
assert.equal(requests.length,2);
open();
const sized=(name,bytes)=>{const file=book(name);Object.defineProperty(file,'size',{value:bytes});return file;};
await choose(book('文楷.TTF'),book('01-夜.jpeg'),sized('big.otf',32*1048576+1),sized('big.png',2*1048576+1),book('OFL.md'));
assert.deepEqual(requests.map(r=>r.url),['/check','/upload','/check','/upload']);
assert.ok(list().includes('已存入 Fonts')&&list().includes('已存入 LockScreens'));
assert.ok(list().includes('字体大小必须为 1 字节至 32 MiB')&&list().includes('锁屏图片大小必须为 1 字节至 2 MiB'));
assert.equal(elements['result-title'].textContent,'已保存 2 个，3 个没有完成');
open();
await choose(sized('max.epub',64*1048576));
assert.equal(requests[1].timeout,120000+64*1048576/50);
const shapes={'wide.png':[4097,10],'huge.jpg':[4096,2049],'edge.png':[4096,2048],'odd.png':null};
open();
context.URL={createObjectURL:file=>file.name,revokeObjectURL(){}};
context.Image=class{set src(name){const shape=shapes[name];
 if(shape){[this.naturalWidth,this.naturalHeight]=shape;this.onload();}else this.onerror();}};
await choose(book('wide.png'),book('huge.jpg'),book('edge.png'),book('odd.png'));
assert.deepEqual(requests.map(r=>r.headers['X-Pico-Name']),['edge.png','edge.png','odd.png','odd.png']);
assert.ok(list().includes('图片为 4097 × 10 像素')&&list().includes('图片为 4096 × 2049 像素'));
open();
let late=null,active=0,overlap=false;
handler=r=>{
 if(r.url==='/upload'){
  overlap=overlap||active>0;active++;
  if(!late){elements.files.files=[book('后加.epub')];late=elements.files.onchange();}
  active--;
 }
 return 200;
};
await choose(book('先传.epub'));await late;
assert.deepEqual(requests.map(r=>r.headers['X-Pico-Name']),['先传.epub','先传.epub','后加.epub','后加.epub'].map(encodeURIComponent));
assert.equal(overlap,false);
assert.equal(elements['result-title'].textContent,'已保存 2 个');
let prevented=false;
context.document.ondragover({preventDefault(){prevented=true;}});
assert.ok(prevented&&elements.drop.className==='drop over');
await context.document.ondrop({preventDefault(){},dataTransfer:{files:[book('拖入.txt')]}});
assert.equal(elements.drop.className,'drop');
assert.equal(requests.length,6);
assert.ok(list().includes('先传.epub')&&list().includes('后加.epub')&&list().includes('拖入.txt'));
assert.equal(elements['result-title'].textContent,'已保存 3 个');
open();
handler=r=>r.url==='/upload'&&r.headers['X-Pico-Name']==='m.epub'&&requests.length<3?'error':200;
await choose(book('m.epub'));
assert.ok(list().includes('data-retry="0"'));
let retried=null;
handler=r=>{if(r.url==='/upload'&&!retried)retried=elements.list.onclick({target:{dataset:{retry:'0'}}});return r.url==='/check'&&r.headers['X-Pico-Name']==='m.epub'?409:200;};
await choose(book('n.epub'));await retried;
assert.deepEqual(requests.slice(2).map(r=>r.headers['X-Pico-Name']+r.url),['n.epub/check','n.epub/upload','m.epub/check']);
assert.ok(list().includes('可能是中断前已保存的这一份')&&!list().includes('data-retry'));
console.log('Wi-Fi page: adding files during and after a transfer passed');
console.log('Wi-Fi page: file kinds and image limits passed');
console.log('Wi-Fi page: per-book rows, refusals, unconfirmed retry, batch stop and escaping passed');

open('zh-Hant');
assert.match(traditionalHtml, /lang="zh-Hant"/);
assert.match(elements.keep.textContent, /^請保持本頁打開。/);
await choose(book('简体书名.epub'),book('图片.pdf'));
assert.deepEqual(requests.map(r=>r.headers['X-Pico-Name']),['简体书名.epub','简体书名.epub'].map(encodeURIComponent));
assert.ok(list().includes('简体书名.epub'));
assert.ok(list().includes('僅支持 EPUB、TXT、TTF、OTF、PNG 和 JPEG 檔案。'));

open('en');
assert.match(englishHtml, /lang="en"/);
assert.equal(elements.route.innerHTML,'Using saved Wi-Fi “home&#60;&#34;x&#34;&#62;”');
assert.equal(elements.keep.textContent,'Keep this page open. Closing it interrupts the current file.');
await choose(book('简体书名.epub'),book('图片.pdf'),book('empty.txt',''));
assert.deepEqual(requests.map(r=>r.headers['X-Pico-Name']),['简体书名.epub','简体书名.epub'].map(encodeURIComponent));
assert.ok(list().includes('简体书名.epub'));
assert.ok(list().includes('Only EPUB, TXT, TTF, OTF, PNG, and JPEG files are supported.'));
assert.ok(list().includes('Allowed size for Books: 1 byte to 64 MiB.'));
assert.equal(elements['result-title'].textContent,'Saved: 1. Not completed: 2.');
open('en');
handler=r=>r.url==='/upload'?'error':200;
await choose(book('原文{saved}.txt'));
assert.ok(list().includes('The result is unconfirmed.')&&list().includes('data-retry="0"'));
assert.equal(requests[0].headers['X-Pico-Name'],encodeURIComponent('原文{saved}.txt'));
assert.equal(elements['result-title'].textContent,'Saved: 0. Not completed: 1.');
