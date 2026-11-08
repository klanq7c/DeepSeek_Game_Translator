'use strict';

// 图片文字属于 RPG Maker 渲染层。识别、坐标和译文均来自当前资源及游戏数据，
// 不包含游戏名称、预制词表、固定矩形或专用译图。原始 Bitmap 始终保持只读。
var fs=require('fs'),path=require('path'),crypto=require('crypto');
var MAX_PIXELS=2097152,MAX_PENDING=16,MAX_OCR_CACHE=64,MAX_OCR_CANDIDATES=512;
function eligible(folder,name){var value=String(folder||'').replace(/\\/g,'/')+String(name||''); return /(?:^|\/)(?:menus?|ui|hud|system|pictures|titles[12]?)(?:\/|$)/i.test(value)&&!/(?:^|\/)(?:faces?|characters|tilesets|animations|battlebacks?[12]?|sv_actors|sv_enemies)(?:\/|$)/i.test(value)&&!/(?:iconset|(?:hp|mp|tp|lv|gold)?(?:meter|number)|particles|cursor|arrow|template|magiccircle)$/i.test(String(name||''));}
function norm(value){value=String(value||'').toLowerCase(); if(value.normalize) value=value.normalize('NFKD'); return value.replace(/[\u0300-\u036f]/g,'').replace(/[^a-z0-9]/g,'').replace(/vv/g,'w');}
function lexicon(system){var result=Object.create(null),count=0;
  function add(text){if(typeof text!=='string'||text.length<3||text.length>80||!/[A-Za-z]/.test(text)||/[\\\x00-\x1f]/.test(text)||count>=1024) return; var key=norm(text); if(!key) return; if(!Object.prototype.hasOwnProperty.call(result,key)){result[key]=text; count++;}else if(result[key]!==text&&result[key].toLowerCase()!==text.toLowerCase()) result[key]=null;}
  function list(values){if(!Array.isArray(values)) return; for(var i=0;i<values.length;i++){var text=values[i]; add(text); if(typeof text!=='string') continue; var words=text.match(/[A-Za-z]{3,}/g)||[]; for(var j=0;j<words.length;j++){add(words[j]); if(!/s$/i.test(words[j])) add(words[j]+'s');}}}
  system=system||{}; var terms=system.terms||{}; list(terms.basic); list(terms.commands); list(terms.params); list(system.equipTypes); list(system.armorTypes); list(system.weaponTypes); list(system.skillTypes); return result;
}
function intersection(a,b){var w=Math.max(0,Math.min(a.x+a.w,b.x+b.w)-Math.max(a.x,b.x)),h=Math.max(0,Math.min(a.y+a.h,b.y+b.h)-Math.max(a.y,b.y)); return w*h/Math.max(1,Math.min(a.w*a.h,b.w*b.h));}
function candidates(pass){var result=[]; var lines=pass&&pass.lines; if(!Array.isArray(lines)||lines.length>256) return result;
  for(var i=0;i<lines.length;i++){var words=lines[i]&&lines[i].words; if(!Array.isArray(words)||words.length>128) continue; var group=null;
    // 双背景需要比较矩形。限制候选数量，避免异常密集结果在游戏帧中产生平方级长循环。
    function flush(){if(group){if(result.length>=MAX_OCR_CANDIDATES)throw new Error('OCR candidate budget exceeded');result.push(group);group=null;}}
    for(var j=0;j<words.length;j++){var word=words[j]; if(!word||typeof word.text!=='string'||word.text.length>100||!isFinite(word.x)||!isFinite(word.y)||!isFinite(word.w)||!isFinite(word.h)||!(word.w>0)||!(word.h>0)) {flush();continue;}
      if(group&&word.x-(group.x+group.w)>Math.max(group.h,word.h)*1.5) flush();
      if(!group) group={text:word.text,x:Number(word.x),y:Number(word.y),w:Number(word.w),h:Number(word.h)};
      else{var right=Math.max(group.x+group.w,word.x+word.w),bottom=Math.max(group.y+group.h,word.y+word.h); group.text+=' '+word.text;group.x=Math.min(group.x,word.x);group.y=Math.min(group.y,word.y);group.w=right-group.x;group.h=bottom-group.y;}
    } flush();
  } return result;
}
function selectRegions(result,dictionary){if(!result||!Array.isArray(result.passes)||result.passes.length!==2) throw new Error('expected two independent OCR passes');
  var passes=[candidates(result.passes[0]),candidates(result.passes[1])],selected=[];
  for(var p=0;p<2;p++) for(var i=0;i<passes[p].length;i++){var candidate=passes[p][i],key=norm(candidate.text),source=dictionary[key],agreement=false;
    if(key.length<3||key.length>80||!/^[A-Za-z0-9 .,'()&!?:+\-]+$/.test(candidate.text)) continue;
    var ambiguous=false;
    for(var j=0;j<passes[1-p].length;j++){var other=passes[1-p][j],otherKey=norm(other.text);if(intersection(candidate,other)>0.65){if(otherKey===key)agreement=true;else if(source&&dictionary[otherKey])ambiguous=true;}}
    if(ambiguous)continue;
    if(!source){if(!agreement)continue;source=candidate.text;}
    var duplicate=false; for(var s=0;s<selected.length;s++) if(intersection(candidate,selected[s])>0.65){duplicate=true;break;}
    if(/^\(/.test(candidate.text)&&/\)$/.test(candidate.text)&&!/^\(/.test(source))source='('+source+')';
    if(!duplicate&&selected.length<64) selected.push({text:source,x:candidate.x,y:candidate.y,w:candidate.w,h:candidate.h});
  } return selected;
}

// 只有两侧均可证明为连续背景的文字区域才覆盖。复杂插画、边框或图标相交时
// 保留该区域原图并记录诊断，不能靠猜测填色隐藏被破坏的素材。
function prepareRegion(canvas,region){var ctx=canvas.getContext('2d'),pad=Math.max(3,Math.ceil(region.h*0.14));
  var x=Math.floor(region.x)-pad,y=Math.floor(region.y)-pad,w=Math.ceil(region.w)+pad*2,h=Math.ceil(region.h)+pad*2;
  if(x<3||y<0||x+w+3>canvas.width||y+h>canvas.height||w<8||h<8||h>160||w>900) return null;
  var pixels=ctx.getImageData(x-3,y,w+6,h),data=pixels.data,stride=(w+6)*4,background=ctx.createImageData(w,h),out=background.data;
  var colors=Object.create(null),best=null;
  for(var row=0;row<h;row++){
    var left=[],right=[];
    for(var c=0;c<4;c++){var l=data[row*stride+c],r=data[row*stride+(w+5)*4+c];
      if(Math.abs(l-r)>18) return null;
      for(var border=1;border<3;border++) if(Math.abs(data[row*stride+border*4+c]-l)>12||Math.abs(data[row*stride+(w+5-border)*4+c]-r)>12) return null;
      left.push(l);right.push(r);
    }
    for(var col=0;col<w;col++){var target=(row*w+col)*4,original=row*stride+(col+3)*4,delta=0;
      for(var channel=0;channel<4;channel++){out[target+channel]=Math.round(left[channel]+(right[channel]-left[channel])*(col+1)/(w+1));delta+=Math.abs(data[original+channel]-out[target+channel]);}
      if(delta>90&&data[original+3]>220){var rgb=[data[original],data[original+1],data[original+2]],bucket=rgb.map(function(v){return Math.round(v/16);}).join(','); var color=colors[bucket]||(colors[bucket]={rgb:rgb,count:0,luma:rgb[0]*0.2126+rgb[1]*0.7152+rgb[2]*0.0722});color.count++;}
    }
  }
  var keys=Object.keys(colors); for(var k=0;k<keys.length;k++){var value=colors[keys[k]];if(value.count>=Math.max(3,Math.floor(w*h*0.002))&&(!best||value.luma>best.luma)) best=value;}
  if(!best) return null;
  return {text:region.text,x:x,y:y,w:w,h:h,background:background,fontSize:Math.max(10,Math.floor(region.h)),color:'rgb('+best.rgb.join(',')+')',outline:best.luma>128?'#252025':'#ffffff'};
}

function workerClient(root,diagnose){var child=null,ready=false,active=null,queue=[],buffer='',timer=null,idle=null,sequence=0,disabled=false;
  function clearTimer(){if(timer){clearTimeout(timer);timer=null;}if(idle){clearTimeout(idle);idle=null;}}
  function fail(error){diagnose('image-ocr-worker',error);disabled=true;clearTimer();var pending=queue;queue=[];if(active){pending.unshift(active);active=null;}var previous=child;child=null;ready=false;if(previous) previous.kill();for(var i=0;i<pending.length;i++)pending[i].callback(error);}
  function pump(){if(!child||!ready||active||!queue.length) return;clearTimer();active=queue.shift();timer=setTimeout(function(){fail(new Error('OCR request exceeded 15 seconds'));},15000);try{child.stdin.write(JSON.stringify({id:active.id,png:active.png})+'\n');}catch(e){fail(e);}}
  function start(){if(child||disabled) return;buffer='';var helper=path.join(root,'js','dst_rpgm_ocr.ps1');
    // Windows PowerShell 5 按系统代码页读取无 BOM 文件；显式按 UTF-8 加载，
    // 避免中文注释尾部被误解码后吞掉下一行。路径仅作为单引号字符串，不能执行插值。
    try{if(!fs.statSync(helper).isFile()) throw new Error('OCR helper is not a file');var bootstrap="$ProgressPreference='SilentlyContinue';& ([scriptblock]::Create([IO.File]::ReadAllText('"+helper.replace(/'/g,"''")+"',[Text.Encoding]::UTF8)))";child=require('child_process').spawn(path.join(process.env.SystemRoot||process.env.WINDIR,'System32','WindowsPowerShell','v1.0','powershell.exe'),['-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-InputFormat','Text','-OutputFormat','Text','-EncodedCommand',Buffer.from(bootstrap,'utf16le').toString('base64')],{windowsHide:true,stdio:['pipe','pipe','pipe']});}
    catch(e){fail(e);return;}
    var current=child;timer=setTimeout(function(){fail(new Error('OCR worker readiness exceeded 8 seconds'));},8000);
    function currentFailure(error){if(current===child)fail(error);}
    child.on('error',currentFailure);child.stdin.on('error',currentFailure);
    child.stderr.on('data',function(chunk){diagnose('image-ocr-system',new Error(String(chunk).slice(0,2048)));});
    child.stdout.on('data',function(chunk){if(current!==child) return;buffer+=String(chunk);if(buffer.length>2097152){fail(new Error('OCR response exceeds 2 MiB'));return;}
      for(var newline=buffer.indexOf('\n');newline>=0;newline=buffer.indexOf('\n')){var line=buffer.slice(0,newline);buffer=buffer.slice(newline+1);var response;
        try{response=JSON.parse(line);}catch(e){fail(e);return;}
        if(!response||typeof response!=='object'||Array.isArray(response)){fail(new Error('OCR response must be a JSON object'));return;}
        if(response.ready===true&&!ready){if(response.version!==1){fail(new Error('OCR worker protocol version mismatch'));return;}clearTimer();ready=true;pump();continue;}
        if(!active||typeof response.id!=='number'||response.id!==active.id){fail(new Error('OCR response ownership mismatch'));return;}
        if(Object.prototype.hasOwnProperty.call(response,'error')){
          if(typeof response.error!=='string'||!response.error||response.error.length>4096){fail(new Error('OCR error response must contain a bounded string'));return;}
        }else if(!response.result||typeof response.result!=='object'||Array.isArray(response.result)){fail(new Error('OCR result response must contain an object'));return;}
        clearTimer();var finished=active;active=null;
        if(response.error) finished.callback(new Error(response.error));else finished.callback(null,response.result);
        pump();if(!active&&!queue.length) idle=setTimeout(function(){if(child===current&&!active&&!queue.length){child=null;ready=false;current.stdin.end();}},10000);
      }
    });
    child.on('exit',function(code){if(current!==child) return;child=null;ready=false;if(active||queue.length)fail(new Error('OCR worker exited with code '+code));});
  }
  return {request:function(png,callback){if(disabled){callback(new Error('OCR worker disabled after an external failure'));return;}if(queue.length+(active?1:0)>=MAX_PENDING){callback(new Error('OCR queue capacity reached'));return;}if(idle){clearTimeout(idle);idle=null;}queue.push({id:++sequence,png:png,callback:callback});start();pump();},close:function(){clearTimer();var current=child;child=null;queue=[];active=null;ready=false;if(current)current.stdin.end();}};
}

function create(options){var diagnose=options.diagnose,Bitmap=options.Bitmap,ImageManager=options.ImageManager,document=options.document;
  var states=new WeakMap(),ocrCache=Object.create(null),cacheOrder=[],inFlight=Object.create(null),dirty=true,lastScene=null,lastEpoch=-1,lastTargets=[],fontReady=false,usableStates=0,closed=false;
  var worker=options.recognize?null:workerClient(options.root,diagnose);
  try{document.fonts.load('16px '+options.font).then(function(faces){if(!faces||!faces.length){diagnose('image-text-font',new Error('CJK font is unavailable'));return;}fontReady=true;dirty=true;},function(error){diagnose('image-text-font',error);});}catch(e){diagnose('image-text-font',e);}
  function canvas(){return document.createElement('canvas');}
  function remember(key,result){ocrCache[key]=result;cacheOrder.push(key);while(cacheOrder.length>MAX_OCR_CACHE)delete ocrCache[cacheOrder.shift()];}
  // 没有成功识别的区域时，任何场景节点都不可能产生本模块的显示目标。
  // 只持有数量而不保留位图；异步识别/销毁负责变更，存在候选时仍逐帧发现动态 Sprite。
  // 未调用 destroy 就被 GC 的位图最多留下保守的非零计数，不会漏掉新目标或保留对象。
  function setRegions(state,regions){if(state.hasRegions)usableStates--;state.regions=regions;state.hasRegions=regions.length>0;if(state.hasRegions)usableStates++;dirty=true;}
  function analyze(state){var original=state.source._canvas||state.source.canvas,png;
    try{png=original.toDataURL('image/png').split(',')[1];if(!png||png.length>2796204)throw new Error('PNG exceeds 2 MiB');}catch(e){diagnose('image-text-snapshot',e);return;}
    var key=crypto.createHash('sha256').update('image-text-v1:').update(png).digest('hex');
    function accept(error,result){if(closed||state.destroyed)return;if(error){diagnose('image-text-recognize',error);return;}try{if(!result||result.width!==original.width||result.height!==original.height)throw new Error('OCR dimensions do not match current bitmap');var regions=selectRegions(result,lexicon(options.system())),prepared=[];for(var i=0;i<regions.length;i++){var region=prepareRegion(original,regions[i]);if(region)prepared.push(region);else diagnose('image-text-background',new Error('background could not be verified for '+regions[i].text));}setRegions(state,prepared);}catch(e){diagnose('image-text-plan',e);}}
    if(Object.prototype.hasOwnProperty.call(ocrCache,key)){accept(null,ocrCache[key]);return;}
    if(inFlight[key]){if(inFlight[key].length<MAX_PENDING)inFlight[key].push(accept);else diagnose('image-text-waiters',new Error('OCR waiter capacity reached'));return;}
    if(Object.keys(inFlight).length>=MAX_PENDING){diagnose('image-text-capacity',new Error('OCR snapshot queue capacity reached'));return;}
    inFlight[key]=[accept];
    function done(error,result){var callbacks=inFlight[key]||[];delete inFlight[key];if(!error)remember(key,result);for(var i=0;i<callbacks.length;i++)callbacks[i](error,result);}
    if(options.recognize)options.recognize(png,done);else worker.request(png,done);
  }
  function wrapLoad(){if(ImageManager.loadBitmap._dsRpgmAutoImageText) return;var old=ImageManager.loadBitmap;
    var wrap=function(folder,name){var source=old.apply(this,arguments);if(!source||!eligible(folder,name)||typeof source.addLoadListener!=='function')return source;
      // 迟加载插件可能包装已安装的入口。内层已返回本层副本时直接复用，
      // 防止再克隆译图或把译文字形交给第二次识别。
      if(source._dsRpgmAutoImageState&&source._dsRpgmAutoImageState.bitmap===source)return source;
      if(states.has(source)&&!states.get(source).destroyed){var cached=states.get(source).bitmap;cached.smooth=source.smooth;return cached;}
      var bitmap=new Bitmap(1,1),state={source:source,bitmap:bitmap,regions:[],applied:Object.create(null),signature:'',ready:false,destroyed:false};
      states.set(source,state);bitmap._dsRpgmAutoImageState=state;bitmap._url=source.url||source._url||'';bitmap._loadingState='requesting';
      if(typeof bitmap.destroy==='function'){var destroy=bitmap.destroy;bitmap.destroy=function(){state.destroyed=true;setRegions(state,[]);if(!state.view)return destroy.apply(this,arguments);};}
      bitmap.isReady=function(){return state.ready;};bitmap.isError=function(){return typeof source.isError==='function'&&source.isError();};bitmap.decode=function(){if(!state.ready&&typeof source.decode==='function')source.decode();};
      source.addLoadListener(function(){if(state.destroyed)return;var w=source.width,h=source.height;
        // 超大图只提供原位图的只读视图，避免为可选识别额外分配完整画布。
        // 图像加载/显存分配错误属于外部渲染器；记录后继续原图，不伪造识别成功。
        if(!(w>0&&h>0)||w*h>MAX_PIXELS){diagnose('image-text-dimensions',new Error('bitmap exceeds image text limits'));originalView(state);}
        else {try{bitmap.resize(w,h);bitmap.blt(source,0,0,w,h,0,0);bitmap.smooth=source.smooth;}catch(e){diagnose('image-text-load',e);originalView(state);state.regions=[];state.ready=true;bitmap._loadingState='loaded';bitmap._callLoadListeners();return;}}
        state.ready=true;bitmap._loadingState='loaded';bitmap._callLoadListeners();dirty=true;if(w>0&&h>0&&w*h<=MAX_PIXELS)analyze(state);
      });
      return bitmap;
    };wrap._dsRpgmAutoImageText=true;wrap._dsRpgmOriginal=old;ImageManager.loadBitmap=wrap;
  }
  function originalView(state){state.view=true;var bitmap=state.bitmap,source=state.source,names=['width','height','_image','_canvas','canvas','_context','context','_baseTexture','baseTexture'];for(var i=0;i<names.length;i++)(function(name){Object.defineProperty(bitmap,name,{configurable:true,get:function(){return source[name];}});})(names[i]);}
  function visit(scene,fn){var seen=new Set(),count=0;function walk(node){if(!node||count>=2048||seen.has(node))return;seen.add(node);count++;var bitmap,children;
    try{if(node.visible===false||node._destroyed)return;bitmap=node.bitmap;children=node.children;}catch(e){diagnose('image-text-scene-node',e);return;}
    if(bitmap&&bitmap._dsRpgmAutoImageState)fn(bitmap._dsRpgmAutoImageState);if(children)for(var i=0;i<children.length;i++)walk(children[i]);}walk(scene);}
  function paint(state){
    if(!state.ready||!state.regions.length||state.destroyed||!fontReady)return;
    var values=[],sizes=[],changed=false,bitmap=state.bitmap,stage,ctx;
    for(var i=0;i<state.regions.length;i++){
      var region=state.regions[i],value=options.translate(region.text);
      if(typeof value!=='string'||value===region.text||!/[\u4e00-\u9fff]/.test(value)||/[\r\n\\\x1b]/.test(value))value=state.applied[i]||null;
      values.push(value);if(value)changed=true;
    }
    if(!changed||JSON.stringify(values)===state.signature)return;
    try{stage=canvas();ctx=stage.getContext('2d');}catch(e){diagnose('image-text-stage',e);return;}
    function fit(region,text){var size=region.fontSize;ctx.font=size+'px '+options.font;while(size>10&&ctx.measureText(text).width>region.w-4){size--;ctx.font=size+'px '+options.font;}return ctx.measureText(text).width<=region.w-2?size:0;}
    // 先验证整张图的显示计划。放不下的新译文只能保留此前已成功显示的值；
    // 不能先写 applied，再以原图重新构建，从而把已有中文擦掉。
    changed=false;
    for(i=0;i<state.regions.length;i++){
      region=state.regions[i];var previous=state.applied[i]||null,size=0;value=values[i];
      if(value){
        try{size=fit(region,value);if(!size){diagnose('image-text-fit',new Error('translation does not fit original text region'));value=previous;if(value){size=fit(region,value);if(!size)return;}}}
        catch(e){diagnose('image-text-measure',e);return;}
      }
      values[i]=value;sizes.push(size);if(value)changed=true;
    }
    var signature=JSON.stringify(values);if(!changed||signature===state.signature)return;
    try{stage.width=state.source.width;stage.height=state.source.height;ctx.drawImage(state.source._canvas||state.source.canvas,0,0);}catch(e){diagnose('image-text-stage',e);return;}
    for(var j=0;j<state.regions.length;j++){
      if(!values[j])continue;var r=state.regions[j];ctx.save();
      try{ctx.font=sizes[j]+'px '+options.font;ctx.beginPath();ctx.rect(r.x,r.y,r.w,r.h);ctx.clip();ctx.putImageData(r.background,r.x,r.y);ctx.textAlign='center';ctx.textBaseline='middle';ctx.lineJoin='round';ctx.lineWidth=2;ctx.strokeStyle=r.outline;ctx.fillStyle=r.color;ctx.strokeText(values[j],r.x+r.w/2,r.y+r.h/2);ctx.fillText(values[j],r.x+r.w/2,r.y+r.h/2);}
      catch(e){diagnose('image-text-region',e);return;}finally{ctx.restore();}
    }
    var destination=bitmap._context||bitmap.context;
    try{destination.save();destination.globalCompositeOperation='copy';destination.drawImage(stage,0,0);if(typeof bitmap._setDirty==='function')bitmap._setDirty();else if(bitmap.baseTexture&&typeof bitmap.baseTexture.update==='function')bitmap.baseTexture.update();else throw new Error('unsupported bitmap texture update API');}catch(e){diagnose('image-text-commit',e);return;}finally{destination.restore();}
    // 只有画布和纹理提交成功后才记录显示值；测量或绘制异常不产生成功状态。
    for(var k=0;k<values.length;k++)if(values[k])state.applied[k]=values[k];
    state.signature=signature;
  }
  // 场景更新只收集当前可见纹理；同场景动态添加/显示 Sprite 也必须被发现。
  // 目标、字体或缓存代次不变时不查译文、不重画；下一帧丢弃离场目标引用。
  function update(scene,epoch){if(closed||usableStates===0){lastTargets=[];lastScene=null;return;}var targets=[],seen=new Set();visit(scene,function(state){if(!seen.has(state)){seen.add(state);targets.push(state);}});
    var changed=targets.length!==lastTargets.length;for(var i=0;!changed&&i<targets.length;i++)changed=targets[i]!==lastTargets[i];
    if(!dirty&&!changed&&lastScene===scene&&lastEpoch===epoch)return;dirty=false;lastScene=scene;lastEpoch=epoch;lastTargets=targets;for(var j=0;j<targets.length;j++)paint(targets[j]);
  }
  return {install:wrapLoad,update:update,hasVisibleTargets:function(scene){if(closed||usableStates===0)return false;var found=false;visit(scene,function(state){if(!state.destroyed&&state.regions.length)found=true;});return found;},refresh:function(){dirty=true;},close:function(){closed=true;lastTargets=[];lastScene=null;if(worker)worker.close();}};
}
module.exports={create:create,eligible:eligible,lexicon:lexicon,selectRegions:selectRegions,prepareRegion:prepareRegion};
