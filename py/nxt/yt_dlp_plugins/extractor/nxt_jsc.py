# NXT Retro Tube: a resident JS challenge solver for yt-dlp.
#
# Videos that only the TV / embedded clients can play ("made for kids" cartoons) carry
# n / signature challenges that must be solved with YouTube's player JS. yt-dlp's built-in
# Deno provider starts a new Deno for every video and re-parses the whole 3 MB player into
# an AST each time; on the A133 that alone takes 15-40 s. This provider keeps ONE Deno
# process alive with the same yt-dlp-ejs solver scripts, prepares each player version once
# (kept in memory, and on disk so the next app start skips the parse), and then answers
# every challenge in milliseconds. If anything goes wrong, yt-dlp falls back to its own
# Deno provider.
# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import json
import os
import select
import subprocess
import sys
import threading
import time

try:
    from yt_dlp.extractor.youtube.jsc._builtin.deno import DenoJCP
    from yt_dlp.extractor.youtube.jsc._builtin.ejs import ScriptVariant
    from yt_dlp.extractor.youtube.jsc.provider import (
        JsChallengeProviderError,
        JsChallengeProviderResponse,
        JsChallengeResponse,
        JsChallengeType,
        NChallengeOutput,
        SigChallengeOutput,
        register_preference,
        register_provider,
    )
except Exception as _e:  # another yt-dlp layout: keep yt-dlp's own solver
    DenoJCP = None
    sys.stderr.write('nxt-jsc: disabled (%s)\n' % _e)

_SERVER_JS = r'''
const CACHE = Deno.args[0];
const ready = new Map();          // player key -> {n, sig}
function prepared(key, pre) {
  const r = {n: null, sig: null};
  Function("_result", pre)(r);
  ready.set(key, r);
  while (ready.size > 2) ready.delete(ready.keys().next().value);
  return r;
}
function fromDisk(key) {
  try { return prepared(key, Deno.readTextFileSync(`${CACHE}/pre-${key}.js`)); } catch (_) { return null; }
}
function toDisk(key, pre) {
  try {
    for (const e of Deno.readDirSync(CACHE)) if (e.name.startsWith("pre-") && e.name !== `pre-${key}.js`) Deno.removeSync(`${CACHE}/${e.name}`);
    Deno.writeTextFileSync(`${CACHE}/pre-${key}.tmp`, pre);
    Deno.renameSync(`${CACHE}/pre-${key}.tmp`, `${CACHE}/pre-${key}.js`);
  } catch (_) {}
}
function handle(req) {
  let s = ready.get(req.key) || fromDisk(req.key);
  if (!s) {
    if (!req.player) return {type: "need_player"};
    const out = jsc({type: "player", player: req.player, requests: [], output_preprocessed: true});
    if (out.type === "error" || !out.preprocessed_player) return {type: "error", error: String(out.error || "no preprocessed player")};
    s = prepared(req.key, out.preprocessed_player);
    toDisk(req.key, out.preprocessed_player);
  }
  const responses = req.requests.map((q) => {
    const f = s[q.type];
    if (!f) return {type: "error", error: `Failed to extract ${q.type} function`};
    try { return {type: "result", data: Object.fromEntries(q.challenges.map((c) => [c, f(c)]))}; }
    catch (e) { return {type: "error", error: e instanceof Error ? `${e.message}` : `${e}`}; }
  });
  return {type: "result", responses};
}
const enc = new TextEncoder();
function send(obj) {
  const b = enc.encode(JSON.stringify(obj) + "\n");
  let off = 0;
  while (off < b.length) off += Deno.stdout.writeSync(b.subarray(off));
}
send({type: "ready"});
const dec = new TextDecoder();
let buf = "";
for await (const chunk of Deno.stdin.readable) {
  buf += dec.decode(chunk, {stream: true});
  let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, i); buf = buf.slice(i + 1);
    if (!line.trim()) continue;
    let out;
    try { out = handle(JSON.parse(line)); } catch (e) { out = {type: "error", error: String(e && e.message || e)}; }
    send(out);
  }
}
'''

_IDLE_EXIT = 600      # seconds without work before the Deno process is stopped (frees ~100 MB)


class _Server:
    def __init__(self):
        self.proc = None
        self.lock = threading.Lock()
        self.last = 0
        self.timer = None
        self.failures = 0

    def _read_line(self, timeout):
        fd = self.proc.stdout.fileno()
        end = time.time() + timeout
        buf = b''
        while not buf.endswith(b'\n'):
            left = end - time.time()
            if left <= 0 or not select.select([fd], [], [], left)[0]:
                raise JsChallengeProviderError('nxt-jsc: timed out')
            chunk = os.read(fd, 65536)
            if not chunk:
                raise JsChallengeProviderError('nxt-jsc: solver exited')
            buf += chunk
        return json.loads(buf.decode())

    def stop(self):
        p, self.proc = self.proc, None
        if p:
            try:
                p.stdin.close()
                p.wait(3)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass

    def _idle_check(self):
        with self.lock:
            if self.proc and time.time() - self.last >= _IDLE_EXIT - 1:
                self.stop()

    def start(self, provider, cache_dir):
        script = os.path.join(cache_dir, 'nxt-jsc-server.js')
        code = '%s\nObject.assign(globalThis, lib);\n%s\n%s' % (provider._lib_script.code, provider._core_script.code, _SERVER_JS)
        old = None
        try:
            with open(script, encoding='utf-8') as f:
                old = f.read()
        except OSError:
            pass
        if old != code:
            with open(script + '.tmp', 'w', encoding='utf-8') as f:
                f.write(code)
            os.replace(script + '.tmp', script)
        cmd = [provider.runtime_info.path, 'run', '--no-prompt', '--no-remote', '--no-lock', '--node-modules-dir=none',
               '--no-config', '--no-npm', '--cached-only', '--allow-read=' + cache_dir, '--allow-write=' + cache_dir,
               script, cache_dir]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     env=provider._get_env_options())
        hello = self._read_line(60)
        if hello.get('type') != 'ready':
            raise JsChallengeProviderError('nxt-jsc: bad start')

    def call(self, provider, cache_dir, req, timeout):
        with self.lock:
            self.last = time.time()
            try:
                if not self.proc or self.proc.poll() is not None:
                    self.start(provider, cache_dir)
                self.proc.stdin.write((json.dumps(req, separators=(',', ':')) + '\n').encode())
                self.proc.stdin.flush()
                out = self._read_line(timeout)
                self.failures = 0
                return out
            except Exception:
                self.failures += 1
                self.stop()
                raise
            finally:
                self.last = time.time()
                if self.timer:
                    self.timer.cancel()
                self.timer = threading.Timer(_IDLE_EXIT, self._idle_check)
                self.timer.daemon = True
                self.timer.start()


_SERVER = _Server()

if DenoJCP is not None:
    @register_provider
    class NxtResidentJCP(DenoJCP):
        PROVIDER_NAME = 'nxt-resident'

        def is_available(self, /) -> bool:
            return _SERVER.failures < 3 and super().is_available()

        def _cache_dir(self):
            root = None
            try:
                root = self.ie._downloader.cache._get_root_dir()
            except Exception:
                pass
            if not root:
                root = os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'yt-dlp')
            d = os.path.join(root, 'nxt-jsc')
            os.makedirs(d, exist_ok=True)
            return d

        def _real_bulk_solve(self, /, requests):
            if self._lib_script.variant == ScriptVariant.DENO_NPM:
                raise JsChallengeProviderError('nxt-jsc: needs the bundled solver scripts')
            cache_dir = self._cache_dir()
            grouped = {}
            for r in requests:
                grouped.setdefault(r.input.player_url, []).append(r)
            for player_url, group in grouped.items():
                # the key also covers the solver version, so an updated yt-dlp re-prepares
                key = hashlib.sha256(('%s|%s|%s' % (player_url, self._lib_script.hash, self._core_script.hash)).encode()).hexdigest()[:24]
                body = [{'type': r.type.value, 'challenges': r.input.challenges} for r in group]
                t0 = time.time()
                out = _SERVER.call(self, cache_dir, {'key': key, 'requests': body}, 30)
                if out.get('type') == 'need_player':
                    video_id = next((r.video_id for r in group), None)
                    player = self._get_player(video_id, player_url)
                    self.logger.info('Preparing player JS once (kept for later videos)')
                    out = _SERVER.call(self, cache_dir, {'key': key, 'requests': body, 'player': player}, 240)
                sys.stderr.write('nxt-jsc: %d challenge set(s) in %.2fs\n' % (len(group), time.time() - t0))
                if out.get('type') != 'result':
                    raise JsChallengeProviderError('nxt-jsc: %s' % out.get('error'))
                for r, data in zip(group, out['responses']):
                    if data.get('type') == 'error':
                        yield JsChallengeProviderResponse(r, None, JsChallengeProviderError(data.get('error')))
                    else:
                        yield JsChallengeProviderResponse(r, JsChallengeResponse(r.type, (
                            NChallengeOutput(data['data']) if r.type is JsChallengeType.N else SigChallengeOutput(data['data']))))

    @register_preference(NxtResidentJCP)
    def _nxt_preference(provider, requests):
        return 2000
