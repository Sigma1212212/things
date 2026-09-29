// ASM3D - a3fs.js
// The file system of the WebAssembly builds: a tree of files kept in JS
// memory, exposed to the engine as the "fs" import module (see
// engine/platform/platform_web.c). The browser page persists /user to
// IndexedDB; the Node test runner keeps everything in memory.

export class A3FS {
  constructor() {
    this.files = new Map();   // absolute path -> { data: Uint8Array, mtime: ms }
    this.dirs = new Set(['/']);
    this.onchange = null;     // (path, kind) callback for persistence
  }
  static norm(p) {
    if (!p.startsWith('/')) p = '/' + p;
    const out = [];
    for (const part of p.split('/')) {
      if (part === '' || part === '.') continue;
      if (part === '..') out.pop(); else out.push(part);
    }
    return '/' + out.join('/');
  }
  static parent(p) { const i = p.lastIndexOf('/'); return i <= 0 ? '/' : p.slice(0, i); }
  mkdirs(p) {
    p = A3FS.norm(p);
    while (p !== '/' && !this.dirs.has(p)) { this.dirs.add(p); p = A3FS.parent(p); }
  }
  write(p, data, mtime) {
    p = A3FS.norm(p);
    this.mkdirs(A3FS.parent(p));
    this.files.set(p, { data, mtime: mtime || Date.now() });
    if (this.onchange) this.onchange(p, 'write');
  }
  read(p) { const f = this.files.get(A3FS.norm(p)); return f ? f.data : null; }
  exists(p) { p = A3FS.norm(p); return this.files.has(p) || this.dirs.has(p); }
  remove(p, recursive) {
    p = A3FS.norm(p);
    if (this.files.delete(p)) { if (this.onchange) this.onchange(p, 'delete'); return true; }
    if (!this.dirs.has(p)) return false;
    const pre = p === '/' ? '/' : p + '/';
    const children = [...this.files.keys()].filter(k => k.startsWith(pre));
    const subdirs = [...this.dirs].filter(d => d.startsWith(pre));
    if (!recursive && (children.length || subdirs.length)) return false;
    for (const k of children) { this.files.delete(k); if (this.onchange) this.onchange(k, 'delete'); }
    for (const d of subdirs) this.dirs.delete(d);
    if (p !== '/') this.dirs.delete(p);
    return true;
  }
  rename(a, b) {
    a = A3FS.norm(a); b = A3FS.norm(b);
    const f = this.files.get(a);
    if (f) { this.files.delete(a); if (this.onchange) this.onchange(a, 'delete'); this.write(b, f.data, f.mtime); return true; }
    if (!this.dirs.has(a)) return false;
    const pre = a + '/';
    for (const [k, v] of [...this.files]) if (k.startsWith(pre)) { this.files.delete(k); if (this.onchange) this.onchange(k, 'delete'); this.write(b + k.slice(a.length), v.data, v.mtime); }
    for (const d of [...this.dirs]) if (d === a || d.startsWith(pre)) { this.dirs.delete(d); this.mkdirs(b + d.slice(a.length)); }
    return true;
  }
  list(p) {
    p = A3FS.norm(p);
    if (!this.dirs.has(p)) return null;
    const pre = p === '/' ? '/' : p + '/';
    const out = new Map();
    for (const d of this.dirs) if (d !== p && d.startsWith(pre) && !d.slice(pre.length).includes('/')) out.set(d.slice(pre.length), { dir: true, size: 0, mtime: 0 });
    for (const [k, v] of this.files) if (k.startsWith(pre) && !k.slice(pre.length).includes('/')) out.set(k.slice(pre.length), { dir: false, size: v.data.length, mtime: v.mtime });
    return [...out.entries()].sort((x, y) => (x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0));
  }
  // The "fs" import object. getMemory() returns the instance's memory.
  imports(getMemory) {
    const dec = new TextDecoder(), enc = new TextEncoder();
    const u8 = () => new Uint8Array(getMemory().buffer);
    const str = (p, n) => dec.decode(u8().subarray(p, p + n));
    return {
      stat: (p, n, mtimePtr) => {
        const path = A3FS.norm(str(p, n));
        const f = this.files.get(path);
        const view = new DataView(getMemory().buffer);
        if (f) { view.setFloat64(mtimePtr, f.mtime, true); return f.data.length; }
        if (this.dirs.has(path)) { view.setFloat64(mtimePtr, 0, true); return -2; }
        return -1;
      },
      read: (p, n, dst, cap) => {
        const d = this.read(str(p, n));
        if (!d) return -1;
        const k = Math.min(cap, d.length);
        u8().set(d.subarray(0, k), dst);
        return k;
      },
      write: (p, n, src, size) => { this.write(str(p, n), u8().slice(src, src + size)); return 1; },
      remove: (p, n, recursive) => (this.remove(str(p, n), !!recursive) ? 1 : 0),
      rename: (a, an, b, bn) => (this.rename(str(a, an), str(b, bn)) ? 1 : 0),
      mkdir: (p, n) => { this.mkdirs(str(p, n)); return 1; },
      list: (p, n, dst, cap) => {
        const entries = this.list(str(p, n));
        if (!entries) return -1;
        const text = entries.map(([name, e]) => `${name}\t${e.size}\t${e.dir ? 1 : 0}\t${e.mtime}\n`).join('');
        const bytes = enc.encode(text);
        if (bytes.length <= cap) u8().set(bytes, dst);
        return bytes.length;
      },
    };
  }
}
