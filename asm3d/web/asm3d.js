// ASM3D - asm3d.js
// Runs the ASM3D editor (asm3d_editor.wasm) in a browser tab:
//   - "gl": OpenGL 3.3 calls from the engine, implemented on WebGL2
//   - "win": the canvas, cursor, pointer lock and clipboard
//   - "a3": clock and logging
//   - "fs": the file system (a3fs.js), with /user kept in IndexedDB
// Browser input events are queued into the engine with a3_web_event().
import { A3FS } from './a3fs.js';

const DB_NAME = 'asm3d-editor';

// ---------------------------------------------------------------- storage

function openDB() {
  return new Promise((resolve) => {
    if (!('indexedDB' in self)) { resolve(null); return; }
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore('files');
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => resolve(null);
  });
}

async function loadPersisted(db, fs) {
  if (!db) return 0;
  return new Promise((resolve) => {
    const tx = db.transaction('files', 'readonly');
    const store = tx.objectStore('files');
    let n = 0;
    const req = store.openCursor();
    req.onsuccess = () => {
      const c = req.result;
      if (!c) { resolve(n); return; }
      const v = c.value;
      fs.write(c.key, new Uint8Array(v.data), v.mtime);
      n++;
      c.continue();
    };
    req.onerror = () => resolve(n);
  });
}

function persistence(db, fs) {
  // write-behind: batch changes to /user every 500 ms
  const pending = new Map();
  let timer = 0;
  const flush = () => {
    timer = 0;
    if (!db || !pending.size) return;
    const tx = db.transaction('files', 'readwrite');
    const store = tx.objectStore('files');
    for (const [path, kind] of pending) {
      if (kind === 'delete') store.delete(path);
      else { const f = fs.files.get(path); if (f) store.put({ data: f.data, mtime: f.mtime }, path); }
    }
    pending.clear();
  };
  fs.onchange = (path, kind) => {
    if (!path.startsWith('/user/')) return;
    pending.set(path, kind);
    if (!timer) timer = setTimeout(flush, 500);
  };
  addEventListener('beforeunload', flush);
  return flush;
}

// ---------------------------------------------------------------- keys

const KEYS = {
  Space: 32, Quote: 39, Comma: 44, Minus: 45, Period: 46, Slash: 47, Semicolon: 59, Equal: 61,
  BracketLeft: 91, Backslash: 92, BracketRight: 93, Backquote: 96,
  Escape: 256, Enter: 257, NumpadEnter: 257, Tab: 258, Backspace: 259, Insert: 260, Delete: 261,
  ArrowRight: 262, ArrowLeft: 263, ArrowDown: 264, ArrowUp: 265, PageUp: 266, PageDown: 267, Home: 268, End: 269,
  CapsLock: 280, ShiftLeft: 340, ControlLeft: 341, AltLeft: 342, MetaLeft: 343,
  ShiftRight: 344, ControlRight: 345, AltRight: 346, MetaRight: 347,
};
for (let i = 0; i < 10; i++) KEYS['Digit' + i] = 48 + i;
for (let i = 0; i < 26; i++) KEYS['Key' + String.fromCharCode(65 + i)] = 65 + i;
for (let i = 1; i <= 12; i++) KEYS['F' + i] = 289 + i;

const EV = { KEY_DOWN: 1, KEY_UP: 2, TEXT: 3, MOUSE_MOVE: 4, MOUSE_DOWN: 5, MOUSE_UP: 6, WHEEL: 7, FOCUS: 8, BLUR: 9, MODS: 10, MOUSE_DELTA: 11, DOUBLE: 12 };
const CURSORS = ['default', 'text', 'pointer', 'ew-resize', 'ns-resize', 'move'];

// ---------------------------------------------------------------- WebGL2

function makeGL(gl, mem, alloc) {
  const u8 = () => new Uint8Array(mem().buffer);
  const i32 = (p, n) => new Int32Array(mem().buffer, p, n);
  const f32 = (p, n) => new Float32Array(mem().buffer, p, n);
  const dv = () => new DataView(mem().buffer);
  const dec = new TextDecoder();
  const cstr = (p) => { const b = u8(); let e = p; while (b[e]) e++; return dec.decode(b.subarray(p, e)); };
  const writeStr = (s, dst, cap, lenPtr) => {
    const bytes = new TextEncoder().encode(s);
    const n = Math.max(0, Math.min(bytes.length, cap - 1));
    if (cap > 0) { u8().set(bytes.subarray(0, n), dst); u8()[dst + n] = 0; }
    if (lenPtr) dv().setInt32(lenPtr, n, true);
  };
  // handle tables (index = GL name the engine sees)
  const T = { buf: [null], tex: [null], vao: [null], fbo: [null], sh: [null], prog: [null], q: [null], loc: [null] };
  const put = (t, o) => { T[t].push(o); return T[t].length - 1; };
  const gen = (t, make) => (n, ptr) => { for (let i = 0; i < n; i++) dv().setUint32(ptr + i * 4, put(t, make()), true); };
  const del = (t, kill) => (n, ptr) => { for (let i = 0; i < n; i++) { const id = dv().getUint32(ptr + i * 4, true); if (T[t][id]) { kill(T[t][id]); T[t][id] = null; } } };
  const progLocs = new Map();   // WebGLProgram -> Map(name -> id)
  let boundFbo = 0;
  const strings = new Map();
  const glString = (s) => { if (!strings.has(s)) { const b = new TextEncoder().encode(s + '\0'); const p = alloc(b.length); u8().set(b, p); strings.set(s, p); } return strings.get(s); };
  const ENABLE_OK = new Set([0x0BE2, 0x0B44, 0x0B71, 0x0C11, 0x8037, 0x0B90, 0x0BD0, 0x8C89, 0x809E, 0x80A0]);
  const typeSize = (t) => (t === 0x1406 || t === 0x1405 || t === 0x1404 ? 4 : t === 0x140B || t === 0x1403 || t === 0x1402 ? 2 : 1);
  const channels = (f) => ({ 0x1903: 1, 0x8227: 2, 0x1907: 3, 0x1908: 4, 0x1902: 1, 0x84F9: 1 }[f] || 4);
  const pixelView = (w, h, format, type, ptr) => {
    if (!ptr) return null;
    const n = w * h * channels(format);
    if (type === 0x1406) return new Float32Array(mem().buffer, ptr, n);
    if (type === 0x1405) return new Uint32Array(mem().buffer, ptr, n);
    if (type === 0x140B) return new Uint16Array(mem().buffer, ptr, n);
    return new Uint8Array(mem().buffer, ptr, n * typeSize(type));
  };
  const fixType = (internal, type, hasData) => {
    if (internal === 0x81A6) return 0x1405;                 // DEPTH_COMPONENT24: UNSIGNED_INT
    if (internal === 0x8CAC) return 0x1406;                 // DEPTH_COMPONENT32F: FLOAT
    if (!hasData && (internal === 0x881A || internal === 0x822D || internal === 0x822F)) return 0x140B; // 16F: HALF_FLOAT
    return type;
  };
  const wrapParam = (v) => (v === 0x812D ? 0x812F : v);     // CLAMP_TO_BORDER -> CLAMP_TO_EDGE
  const G = {
    glGetError: () => gl.getError(),
    glGetString: (name) => glString({ 0x1F00: 'ASM3D WebGL2', 0x1F01: gl.getParameter(gl.RENDERER) || 'WebGL2', 0x1F02: 'OpenGL ES 3.0 (WebGL 2.0)', 0x8B8C: 'GLSL ES 3.00' }[name] || ''),
    glGetStringi: () => glString(''),
    glGetIntegerv: (pname, ptr) => {
      if (pname === 0x8CA6) { dv().setInt32(ptr, boundFbo, true); return; }   // FRAMEBUFFER_BINDING
      if (pname === 0x821D) { dv().setInt32(ptr, 0, true); return; }          // NUM_EXTENSIONS
      let v = null;
      try { v = gl.getParameter(pname); } catch (e) { v = null; }
      if (v === null || v === undefined) { dv().setInt32(ptr, 0, true); return; }
      if (typeof v === 'number' || typeof v === 'boolean') dv().setInt32(ptr, Number(v), true);
      else if (v.length !== undefined) for (let i = 0; i < v.length; i++) dv().setInt32(ptr + i * 4, v[i], true);
    },
    glEnable: (c) => { if (ENABLE_OK.has(c)) gl.enable(c); },
    glDisable: (c) => { if (ENABLE_OK.has(c)) gl.disable(c); },
    glBlendFunc: (a, b) => gl.blendFunc(a, b),
    glBlendFuncSeparate: (a, b, c, d) => gl.blendFuncSeparate(a, b, c, d),
    glDepthFunc: (f) => gl.depthFunc(f),
    glDepthMask: (m) => gl.depthMask(!!m),
    glCullFace: (m) => gl.cullFace(m),
    glFrontFace: (m) => gl.frontFace(m),
    glPolygonMode: () => {},                                  // no wireframe in WebGL
    glPolygonOffset: (a, b) => gl.polygonOffset(a, b),
    glColorMask: (r, g, b, a) => gl.colorMask(!!r, !!g, !!b, !!a),
    glClear: (m) => gl.clear(m),
    glClearColor: (r, g, b, a) => gl.clearColor(r, g, b, a),
    glClearDepth: (d) => gl.clearDepth(d),
    glViewport: (x, y, w, h) => gl.viewport(x, y, w, h),
    glScissor: (x, y, w, h) => gl.scissor(x, y, w, h),
    glPixelStorei: (p, v) => { if (p === 0x0CF5 || p === 0x0D05) gl.pixelStorei(p, v); },
    glReadPixels: (x, y, w, h, format, type, ptr) => gl.readPixels(x, y, w, h, format, type, new Uint8Array(mem().buffer, ptr, w * h * channels(format) * typeSize(type))),
    glFinish: () => gl.finish(),
    glFlush: () => gl.flush(),
    glLineWidth: (w) => gl.lineWidth(w),
    glGenBuffers: gen('buf', () => gl.createBuffer()),
    glDeleteBuffers: del('buf', (o) => gl.deleteBuffer(o)),
    glBindBuffer: (t, id) => gl.bindBuffer(t, T.buf[id] || null),
    glBufferData: (t, size, ptr, usage) => { if (ptr) gl.bufferData(t, u8().subarray(ptr, ptr + size), usage); else gl.bufferData(t, size, usage); },
    glBufferSubData: (t, off, size, ptr) => gl.bufferSubData(t, off, u8().subarray(ptr, ptr + size)),
    glBindBufferBase: (t, i, id) => gl.bindBufferBase(t, i, T.buf[id] || null),
    glGenVertexArrays: gen('vao', () => gl.createVertexArray()),
    glDeleteVertexArrays: del('vao', (o) => gl.deleteVertexArray(o)),
    glBindVertexArray: (id) => gl.bindVertexArray(T.vao[id] || null),
    glEnableVertexAttribArray: (i) => gl.enableVertexAttribArray(i),
    glDisableVertexAttribArray: (i) => gl.disableVertexAttribArray(i),
    glVertexAttribPointer: (i, n, type, norm, stride, off) => gl.vertexAttribPointer(i, n, type, !!norm, stride, off),
    glVertexAttribIPointer: (i, n, type, stride, off) => gl.vertexAttribIPointer(i, n, type, stride, off),
    glVertexAttribDivisor: (i, d) => gl.vertexAttribDivisor(i, d),
    glGenTextures: gen('tex', () => gl.createTexture()),
    glDeleteTextures: del('tex', (o) => gl.deleteTexture(o)),
    glBindTexture: (t, id) => gl.bindTexture(t, T.tex[id] || null),
    glActiveTexture: (u) => gl.activeTexture(u),
    glTexImage2D: (t, level, internal, w, h, border, format, type, ptr) => {
      if (internal === 0x1902) internal = 0x81A6;            // unsized depth -> DEPTH_COMPONENT24
      type = fixType(internal, type, !!ptr);
      gl.texImage2D(t, level, internal, w, h, 0, format, type, pixelView(w, h, format, type, ptr));
    },
    glTexSubImage2D: (t, level, x, y, w, h, format, type, ptr) => gl.texSubImage2D(t, level, x, y, w, h, format, type, pixelView(w, h, format, type, ptr)),
    glTexParameteri: (t, p, v) => { if (p === 0x8501) return; gl.texParameteri(t, p, wrapParam(v)); },
    glTexParameterf: (t, p, v) => { if (p === 0x84FE) return; gl.texParameterf(t, p, v); },
    glTexParameterfv: () => {},                               // border color: not in WebGL
    glGenerateMipmap: (t) => gl.generateMipmap(t),
    glCreateShader: (type) => put('sh', gl.createShader(type)),
    glDeleteShader: (id) => { if (T.sh[id]) { gl.deleteShader(T.sh[id]); T.sh[id] = null; } },
    glShaderSource: (id, count, strs, lens) => {
      let src = '';
      for (let i = 0; i < count; i++) {
        const p = dv().getUint32(strs + i * 4, true);
        const n = lens ? dv().getInt32(lens + i * 4, true) : -1;
        src += n >= 0 ? dec.decode(u8().subarray(p, p + n)) : cstr(p);
      }
      gl.shaderSource(T.sh[id], src);
    },
    glCompileShader: (id) => gl.compileShader(T.sh[id]),
    glGetShaderiv: (id, pname, ptr) => {
      const s = T.sh[id];
      const v = pname === 0x8B84 ? (gl.getShaderInfoLog(s) || '').length + 1 : gl.getShaderParameter(s, pname);
      dv().setInt32(ptr, Number(v), true);
    },
    glGetShaderInfoLog: (id, cap, lenPtr, dst) => writeStr(gl.getShaderInfoLog(T.sh[id]) || '', dst, cap, lenPtr),
    glCreateProgram: () => put('prog', gl.createProgram()),
    glDeleteProgram: (id) => { if (T.prog[id]) { gl.deleteProgram(T.prog[id]); progLocs.delete(T.prog[id]); T.prog[id] = null; } },
    glAttachShader: (p, s) => gl.attachShader(T.prog[p], T.sh[s]),
    glDetachShader: (p, s) => gl.detachShader(T.prog[p], T.sh[s]),
    glLinkProgram: (p) => gl.linkProgram(T.prog[p]),
    glGetProgramiv: (id, pname, ptr) => {
      const p = T.prog[id];
      const v = pname === 0x8B84 ? (gl.getProgramInfoLog(p) || '').length + 1 : gl.getProgramParameter(p, pname);
      dv().setInt32(ptr, Number(v), true);
    },
    glGetProgramInfoLog: (id, cap, lenPtr, dst) => writeStr(gl.getProgramInfoLog(T.prog[id]) || '', dst, cap, lenPtr),
    glUseProgram: (id) => gl.useProgram(T.prog[id] || null),
    glBindAttribLocation: (p, i, name) => gl.bindAttribLocation(T.prog[p], i, cstr(name)),
    glGetUniformLocation: (id, namePtr) => {
      const p = T.prog[id];
      if (!p) return -1;
      const name = cstr(namePtr);
      let m = progLocs.get(p);
      if (!m) { m = new Map(); progLocs.set(p, m); }
      if (m.has(name)) return m.get(name);
      const loc = gl.getUniformLocation(p, name);
      const v = loc ? put('loc', loc) : -1;
      m.set(name, v);
      return v;
    },
    glGetUniformBlockIndex: (p, name) => gl.getUniformBlockIndex(T.prog[p], cstr(name)),
    glUniformBlockBinding: (p, i, b) => gl.uniformBlockBinding(T.prog[p], i, b),
    glUniform1i: (l, v) => { if (l > 0) gl.uniform1i(T.loc[l], v); },
    glUniform1f: (l, v) => { if (l > 0) gl.uniform1f(T.loc[l], v); },
    glUniform2f: (l, a, b) => { if (l > 0) gl.uniform2f(T.loc[l], a, b); },
    glUniform3f: (l, a, b, c) => { if (l > 0) gl.uniform3f(T.loc[l], a, b, c); },
    glUniform4f: (l, a, b, c, d) => { if (l > 0) gl.uniform4f(T.loc[l], a, b, c, d); },
    glUniform1fv: (l, n, p) => { if (l > 0) gl.uniform1fv(T.loc[l], f32(p, n)); },
    glUniform2fv: (l, n, p) => { if (l > 0) gl.uniform2fv(T.loc[l], f32(p, n * 2)); },
    glUniform3fv: (l, n, p) => { if (l > 0) gl.uniform3fv(T.loc[l], f32(p, n * 3)); },
    glUniform4fv: (l, n, p) => { if (l > 0) gl.uniform4fv(T.loc[l], f32(p, n * 4)); },
    glUniform1iv: (l, n, p) => { if (l > 0) gl.uniform1iv(T.loc[l], i32(p, n)); },
    glUniformMatrix4fv: (l, n, tr, p) => { if (l > 0) gl.uniformMatrix4fv(T.loc[l], !!tr, f32(p, n * 16)); },
    glGenFramebuffers: gen('fbo', () => gl.createFramebuffer()),
    glDeleteFramebuffers: del('fbo', (o) => gl.deleteFramebuffer(o)),
    glBindFramebuffer: (t, id) => { gl.bindFramebuffer(t, T.fbo[id] || null); if (t === 0x8D40 || t === 0x8CA9) boundFbo = id; },
    glFramebufferTexture2D: (t, att, tt, tex, level) => gl.framebufferTexture2D(t, att, tt, T.tex[tex] || null, level),
    glCheckFramebufferStatus: (t) => gl.checkFramebufferStatus(t),
    glDrawBuffers: (n, ptr) => {
      if (!boundFbo) { gl.drawBuffers([n > 0 && dv().getUint32(ptr, true) !== 0 ? gl.BACK : gl.NONE]); return; }
      const a = []; for (let i = 0; i < n; i++) a.push(dv().getUint32(ptr + i * 4, true));
      gl.drawBuffers(a);
    },
    glReadBuffer: (s) => gl.readBuffer(boundFbo ? s : (s ? gl.BACK : gl.NONE)),
    glBlitFramebuffer: (a, b, c, d, e, f, g, h, m, flt) => gl.blitFramebuffer(a, b, c, d, e, f, g, h, m, flt),
    glDrawArrays: (m, f, c) => gl.drawArrays(m, f, c),
    glDrawElements: (m, c, t, off) => gl.drawElements(m, c, t, off),
    glDrawArraysInstanced: (m, f, c, n) => gl.drawArraysInstanced(m, f, c, n),
    glDrawElementsInstanced: (m, c, t, off, n) => gl.drawElementsInstanced(m, c, t, off, n),
    // timer queries: not available everywhere in WebGL2; report "no data"
    glGenQueries: (n, ptr) => { for (let i = 0; i < n; i++) dv().setUint32(ptr + i * 4, put('q', {}), true); },
    glDeleteQueries: () => {},
    glQueryCounter: () => {},
    glBeginQuery: () => {},
    glEndQuery: () => {},
    glGetQueryObjectiv: (id, pname, ptr) => dv().setInt32(ptr, 1, true),
    glGetQueryObjectui64v: (id, pname, ptr) => { dv().setUint32(ptr, 0, true); dv().setUint32(ptr + 4, 0, true); },
  };
  return G;
}

// ---------------------------------------------------------------- start

export async function startEditor(opts) {
  const canvas = opts.canvas;
  const status = opts.status || (() => {});
  const log = opts.log || ((lvl, s) => (lvl >= 4 ? console.error : lvl === 3 ? console.warn : console.log)(s));
  status('Loading file storage...');
  const fs = new A3FS();
  const db = opts.persist === false ? null : await openDB();
  const restored = await loadPersisted(db, fs);
  const flush = persistence(db, fs);
  fs.mkdirs('/user/projects');
  if (opts.files) for (const [p, d] of Object.entries(opts.files)) fs.write(p, d);

  status('Starting WebGL2...');
  const gl = canvas.getContext('webgl2', { antialias: false, alpha: false, depth: true, stencil: false, preserveDrawingBuffer: true, powerPreference: 'high-performance' });
  if (!gl) throw new Error('This browser does not support WebGL2.');
  gl.getExtension('EXT_color_buffer_float');
  gl.getExtension('OES_texture_float_linear');

  let memory = null, exports = null;
  const mem = () => memory;
  const dec = new TextDecoder();
  const str = (p, n) => dec.decode(new Uint8Array(memory.buffer, p, n));
  let clipboard = '';
  const G = makeGL(gl, mem, (n) => exports.a3_web_alloc(n));
  const audio = { ctx: null, node: null };
  const api = { fs, exports: null, flush, restored, exitCode: 0, running: false, frames: 0 };
  // browsers only start sound after a user gesture
  const unlock = () => { if (audio.ctx && audio.ctx.state !== 'running') audio.ctx.resume().catch(() => {}); };
  addEventListener('mousedown', unlock);
  addEventListener('keydown', unlock);

  const resize = () => {
    const r = canvas.getBoundingClientRect();
    const w = Math.max(320, Math.floor(r.width)), h = Math.max(240, Math.floor(r.height));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  };
  resize();
  addEventListener('resize', resize);

  const imports = {
    a3: {
      log: (lvl, p, n) => log(lvl, str(p, n)),
      now_ms: () => performance.now(),
      wall_clock: () => Date.now() / 1000,
      abort: (p, n) => { throw new Error('ASM3D aborted: ' + str(p, n)); },
    },
    fs: fs.imports(mem),
    win: {
      gl_init: () => 1,
      canvas_width: () => canvas.width,
      canvas_height: () => canvas.height,
      set_title: (p, n) => { document.title = str(p, n) + ' - ASM3D (browser)'; },
      set_cursor: (c) => { canvas.style.cursor = CURSORS[c] || 'default'; },
      capture_mouse: (on) => { if (on) canvas.requestPointerLock && canvas.requestPointerLock(); else if (document.pointerLockElement) document.exitPointerLock(); },
      set_clipboard: (p, n) => { clipboard = str(p, n); if (navigator.clipboard) navigator.clipboard.writeText(clipboard).catch(() => {}); },
      get_clipboard: (dst, cap) => { const b = new TextEncoder().encode(clipboard); const k = Math.min(cap, b.length); new Uint8Array(memory.buffer).set(b.subarray(0, k), dst); return k; },
    },
    gl: G,
    audio: {
      open: (rate) => {
        const AC = self.AudioContext || self.webkitAudioContext;
        if (!AC) return 0;
        try { audio.ctx = new AC({ sampleRate: rate }); } catch (e) { try { audio.ctx = new AC(); } catch (e2) { return 0; } }
        const ctx = audio.ctx;
        const node = ctx.createScriptProcessor(2048, 0, 2);
        node.onaudioprocess = (e) => {
          const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1);
          if (!exports || !api.running) { L.fill(0); R.fill(0); return; }
          const n = L.length;
          const p = exports.a3_web_audio_render(n);
          const buf = new Float32Array(memory.buffer, p, n * 2);
          for (let i = 0; i < n; i++) { L[i] = buf[i * 2]; R[i] = buf[i * 2 + 1]; }
        };
        node.connect(ctx.destination);
        audio.node = node;
        return ctx.sampleRate | 0;
      },
      close: () => { if (audio.node) audio.node.disconnect(); if (audio.ctx) audio.ctx.close(); audio.ctx = audio.node = null; },
    },
  };

  status('Downloading the editor...');
  const resp = await fetch(opts.wasm || 'asm3d_editor.wasm');
  if (!resp.ok) throw new Error('Could not download asm3d_editor.wasm (' + resp.status + ')');
  const bytes = await resp.arrayBuffer();
  status('Compiling...');
  const { instance } = await WebAssembly.instantiate(bytes, imports);
  exports = instance.exports;
  memory = exports.memory;

  // ---- input ----
  const ev = (type, a = 0, x = 0, y = 0) => exports.a3_web_event(type, a, x, y);
  const mods = (e) => (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
  const pos = (e) => { const r = canvas.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  canvas.tabIndex = 0;
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.addEventListener('mousemove', (e) => {
    if (document.pointerLockElement === canvas) ev(EV.MOUSE_DELTA, 0, e.movementX, e.movementY);
    else { const [x, y] = pos(e); ev(EV.MOUSE_MOVE, 0, x, y); }
  });
  const btn = (b) => (b === 0 ? 0 : b === 2 ? 1 : b === 1 ? 2 : -1);
  canvas.addEventListener('mousedown', (e) => { canvas.focus(); ev(EV.MODS, mods(e)); const b = btn(e.button); if (b >= 0) ev(EV.MOUSE_DOWN, b); e.preventDefault(); });
  addEventListener('mouseup', (e) => { const b = btn(e.button); if (b >= 0) ev(EV.MOUSE_UP, b); });
  canvas.addEventListener('dblclick', (e) => { const b = btn(e.button); if (b >= 0) ev(EV.DOUBLE, b); });
  canvas.addEventListener('wheel', (e) => { ev(EV.WHEEL, 0, -Math.sign(e.deltaX), -Math.sign(e.deltaY)); e.preventDefault(); }, { passive: false });
  canvas.addEventListener('keydown', (e) => {
    ev(EV.MODS, mods(e));
    const k = KEYS[e.code];
    if (k) ev(EV.KEY_DOWN, k);
    if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) ev(EV.TEXT, e.key.codePointAt(0));
    // keep the browser from stealing editor shortcuts (Ctrl+S, Tab, F5, ...)
    if (k && (e.ctrlKey || e.metaKey || k >= 256 || e.code === 'Space')) {
      if (!((e.ctrlKey || e.metaKey) && (e.code === 'KeyV' || e.code === 'KeyC' || e.code === 'KeyX'))) e.preventDefault();
    }
  });
  canvas.addEventListener('keyup', (e) => { ev(EV.MODS, mods(e)); const k = KEYS[e.code]; if (k) ev(EV.KEY_UP, k); });
  canvas.addEventListener('paste', (e) => { clipboard = e.clipboardData.getData('text') || clipboard; });
  addEventListener('paste', (e) => { clipboard = (e.clipboardData && e.clipboardData.getData('text')) || clipboard; });
  canvas.addEventListener('focus', () => ev(EV.FOCUS));
  canvas.addEventListener('blur', () => ev(EV.BLUR));
  document.addEventListener('pointerlockchange', () => { if (document.pointerLockElement !== canvas) exports.a3_web_capture_lost(); });

  // ---- run ----
  status('');
  const argBytes = new TextEncoder().encode((opts.args || '') + '\0');
  const argPtr = exports.a3_web_alloc(argBytes.length);
  new Uint8Array(memory.buffer).set(argBytes, argPtr);
  api.exports = exports;
  api.running = true;             // the audio callback may run during start
  const rc = exports.a3_web_editor_start(argPtr);
  api.exitCode = rc;
  api.running = rc < 0;
  if (rc >= 0) { flush(); return api; }
  canvas.focus();
  const frame = () => {
    if (!api.running) return;
    resize();
    try {
      if (!exports.a3_web_editor_frame()) { api.running = false; flush(); return; }
    } catch (e) {
      api.running = false;
      log(5, 'The editor stopped: ' + e.message);
      throw e;
    }
    api.frames++;
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  return api;
}

// ---------------------------------------------------------------- zip (store only)
// Projects can be downloaded as .zip and opened again (no compression).

const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
function crc32(d) { let c = 0xFFFFFFFF; for (let i = 0; i < d.length; i++) c = CRC[(c ^ d[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

export function zipFiles(entries) {   // [[name, Uint8Array], ...]
  const enc = new TextEncoder();
  const parts = [], central = [];
  let offset = 0;
  for (const [name, data] of entries) {
    const nb = enc.encode(name), crc = crc32(data);
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(8, 0, true);
    h.setUint32(14, crc, true); h.setUint32(18, data.length, true); h.setUint32(22, data.length, true); h.setUint16(26, nb.length, true);
    parts.push(new Uint8Array(h.buffer), nb, data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true);
    c.setUint32(16, crc, true); c.setUint32(20, data.length, true); c.setUint32(24, data.length, true); c.setUint16(28, nb.length, true); c.setUint32(42, offset, true);
    central.push(new Uint8Array(c.buffer), nb);
    offset += 30 + nb.length + data.length;
  }
  const csize = central.reduce((s, p) => s + p.length, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, entries.length, true); e.setUint16(10, entries.length, true);
  e.setUint32(12, csize, true); e.setUint32(16, offset, true);
  return new Blob([...parts, ...central, new Uint8Array(e.buffer)], { type: 'application/zip' });
}

export async function unzipFiles(buf) {   // stored and deflated entries
  const d = new DataView(buf), u8 = new Uint8Array(buf), dec = new TextDecoder();
  let eocd = -1;
  for (let i = buf.byteLength - 22; i >= 0; i--) if (d.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error('not a zip file');
  const count = d.getUint16(eocd + 10, true);
  let p = d.getUint32(eocd + 16, true);
  const out = [];
  for (let i = 0; i < count; i++) {
    const method = d.getUint16(p + 10, true), csize = d.getUint32(p + 20, true);
    const nlen = d.getUint16(p + 28, true), xlen = d.getUint16(p + 30, true), clen = d.getUint16(p + 32, true);
    const lho = d.getUint32(p + 42, true);
    const name = dec.decode(u8.subarray(p + 46, p + 46 + nlen));
    p += 46 + nlen + xlen + clen;
    if (name.endsWith('/')) continue;
    const start = lho + 30 + d.getUint16(lho + 26, true) + d.getUint16(lho + 28, true);
    let data = u8.slice(start, start + csize);
    if (method === 8) data = new Uint8Array(await new Response(new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer());
    else if (method !== 0) throw new Error('unsupported zip compression in ' + name);
    out.push([name, data]);
  }
  return out;
}
