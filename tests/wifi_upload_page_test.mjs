// Executes the shipped browser script with a small DOM and transport stand-in.
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=readFileSync(new URL('../firmware/components/platform_read_pico/upload_page.h',import.meta.url),'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements=Object.fromEntries(['files','send','cancel','progress','status'].map(id=>[id,{}]));
let code=200,requests=[];
class Request {
 constructor(){this.upload={};this.headers={};}
 open(method,url){assert.equal(method,'POST');assert.ok(['/check','/upload'].includes(url));this.url=url;}
 setRequestHeader(k,v){this.headers[k]=v;}
 send(file){requests.push(this);this.status=code;this.responseText=code===200?'已保存':'同名文件';this.onload();}
}
const context=vm.createContext({document:{getElementById:id=>elements[id]},XMLHttpRequest:Request,Uint8Array,token:'test'});
vm.runInContext(script,context);
const file=new Blob(['123456789']);file.name='测试.epub';elements.files.files=[file];
await elements.send.onclick();
assert.equal(requests[0].url,'/check');assert.equal(requests[1].url,'/upload');
assert.equal(requests[0].headers['X-Pico-Size'],'9');
assert.equal(requests[0].headers['X-Pico-CRC'],'cbf43926');
assert.equal(requests[0].headers['X-Pico-Name'],encodeURIComponent(file.name));
assert.equal(requests[0].headers['X-Pico-Token'],'test');
assert.match(elements.status.textContent,/全部保存/);
file.name='测试.txt';code=200;await elements.send.onclick();
assert.equal(requests.at(-2).headers['X-Pico-Name'],encodeURIComponent(file.name));
assert.equal(requests.at(-1).url,'/upload');
code=400;const beforeReject=requests.length;await elements.send.onclick();assert.equal(elements.status.textContent,'同名文件');
assert.equal(requests.length,beforeReject+1);assert.equal(requests.at(-1).url,'/check');
file.name='../bad.pdf';const count=requests.length;await elements.send.onclick();assert.equal(requests.length,count);
assert.equal(elements.send.disabled,false);assert.equal(elements.cancel.disabled,true);
console.log('Wi-Fi page: EPUB/TXT, CRC, filename, error feedback and invalid extension passed');
