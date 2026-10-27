const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
(async () => {
 const root=__dirname;
 const names={PreviousSource:'rewind',NextSource:'fast-forward',Close:'x',PreviousPage:'caret-left',NextPage:'caret-right',Expand:'arrows-out-simple',Collapse:'arrows-in-simple'};
 let header='// Generated from the unmodified Phosphor SVG files in this directory.\n// MIT license and pinned source are recorded in LICENSE and README.md.\n#pragma once\n#include <cstdint>\n\nnamespace pico_page::dictionary_icons {\ninline constexpr int kSize = 40;\n';
 for (const [name,asset] of Object.entries(names)) {
  const {data,info}=await sharp(path.join(root,asset+'.svg')).resize(40,40).ensureAlpha().raw().toBuffer({resolveWithObject:true});
  const packed=[];
  for(let i=0;i<1600;i+=2) packed.push(Math.round(data[i*info.channels+3]/17)|(Math.round(data[(i+1)*info.channels+3]/17)<<4));
  header+='inline constexpr std::uint8_t k'+name+'[] = {\n';
  for(let i=0;i<packed.length;i+=20)header+='    '+packed.slice(i,i+20).map(v=>'0x'+v.toString(16).padStart(2,'0')).join(', ')+',\n';
  header+='};\n';
 }
 header+='}  // namespace pico_page::dictionary_icons\n';
 fs.writeFileSync(path.join(root,'dictionary_icons.h'),header);
})();
