# NXT Retro Tube helper: keeps one yt-dlp process alive and answers requests
# on stdin/stdout, so Python start-up and YouTube set-up are paid only once.
# Request (one JSON per line):  {"op": "resolve"|"search"|"channel"|"ping", ...}
# Reply (one line):             @@NXT {"ok": true, "data": {...}} | {"ok": false, "error": "..."}
# SPDX-License-Identifier: GPL-3.0-only
import json
import os
import sys
import time

from yt_dlp.extractor.common import InfoExtractor

FORMAT_KEYS = ('format_id', 'url', 'protocol', 'vcodec', 'acodec', 'height', 'width', 'fps', 'tbr', 'abr',
               'filesize', 'filesize_approx', 'format_note', 'language', 'http_headers', 'ext')
ENTRY_KEYS = ('id', 'title', 'channel', 'uploader', 'channel_id', 'uploader_id', 'duration', 'view_count',
              'live_status', 'is_live', '_type', 'ie_key')
INFO_KEYS = ENTRY_KEYS + ('like_count', 'upload_date', 'description', 'http_headers')


def _reply(obj):
    data = ('@@NXT ' + json.dumps(obj, ensure_ascii=True, separators=(',', ':')) + '\n').encode()
    view = memoryview(data)
    while view:
        n = os.write(1, view)
        view = view[n:]


class NxtTubeServerIE(InfoExtractor):
    IE_NAME = 'nxttube'
    IE_DESC = False
    _VALID_URL = r'nxttube:(?P<id>serve)'

    def _run(self, ydl, url, params):
        params = dict(params, ignoreerrors=False)
        saved = {k: ydl.params.get(k) for k in params}
        ydl.params.update(params)
        try:
            return ydl.sanitize_info(ydl.extract_info(url, download=False))
        finally:
            ydl.params.update(saved)

    def _resolve(self, ydl, vid, clients, visitor=None):
        url = 'https://www.youtube.com/watch?v=' + vid
        last = None
        for client in clients:
            # visionos: no JS player, no PO token. Without a visitor id YouTube asks to sign in,
            # so either pass the app's visitor id or let yt-dlp read it from the watch page.
            if client == 'visionos' and visitor:
                args = {'youtube': {'player_client': ['visionos'], 'player_skip': ['webpage', 'configs'], 'visitor_data': [visitor]}}
            elif client == 'visionos':
                continue
            elif client == 'visionos-page':
                args = {'youtube': {'player_client': ['visionos'], 'player_skip': ['configs']}}
            elif client == 'default':
                args = {}
            else:
                # TV / embedded / Safari clients: they work for "made for kids" videos (cartoons)
                # that visionOS refuses; they need the JS player, solved with Deno
                args = {'youtube': {'player_client': [client]}}
            t0 = time.time()
            try:
                info = self._run(ydl, url, {'extractor_args': args, 'noplaylist': True, 'extract_flat': False,
                                           'format': 'bv*+ba/b', 'ignore_no_formats_error': True})
            except Exception as e:  # try the next client
                last = e
                sys.stderr.write('client %s failed after %.1fs: %s\n' % (client, time.time() - t0, e))
                continue
            formats = [f for f in info.get('formats') or [] if f.get('url')]
            sys.stderr.write('client %s: %d formats in %.1fs\n' % (client, len(formats), time.time() - t0))
            if formats:
                out = {k: info.get(k) for k in INFO_KEYS if info.get(k) is not None}
                if out.get('description'):
                    out['description'] = out['description'][:600]
                out['formats'] = [{k: f.get(k) for k in FORMAT_KEYS if f.get(k) is not None} for f in formats]
                out['nxt_captions'] = self._captions(info)
                out['nxt_client'] = client
                return out
            last = Exception(info.get('_nxt_error') or 'No playable formats')
        raise last or Exception('No playable formats')

    @staticmethod
    def _captions(info):
        # original-language tracks only (json3); the app adds &tlang=<lang> for YouTube's translation
        def json3(lst):
            for f in lst or []:
                if f.get('ext') == 'json3' and f.get('url'):
                    return f['url']
            return None
        out = []
        for lang, lst in (info.get('subtitles') or {}).items():
            url = json3(lst)
            if url and lang != 'live_chat':
                out.append({'lang': lang, 'name': (lst[0].get('name') or lang), 'url': url, 'asr': False, 'translatable': True})
        auto = info.get('automatic_captions') or {}
        origs = [k for k in auto if k.endswith('-orig')]
        if not origs:   # no translation list (webpage skipped): the untranslated tracks
            origs = [k for k in auto if (json3(auto[k]) or '').find('tlang=') < 0]
        for key in origs:
            url = json3(auto[key])
            if url:
                lang = key[:-5] if key.endswith('-orig') else key
                out.append({'lang': lang, 'name': (auto[key][0].get('name') or lang), 'url': url, 'asr': True, 'translatable': True})
        return out[:16]

    def _fetch(self, ydl, url):
        from yt_dlp.networking import Request
        try:
            from yt_dlp.networking.impersonate import ImpersonateTarget
            resp = ydl.urlopen(Request(url, extensions={'impersonate': ImpersonateTarget()}))
        except Exception:
            resp = ydl.urlopen(Request(url))
        return {'text': resp.read().decode('utf-8', 'replace')}

    def _list(self, ydl, url, count, start=0):
        info = self._run(ydl, url, {'extract_flat': 'in_playlist', 'playliststart': start + 1, 'playlistend': start + count,
                                   'extractor_args': {}, 'noplaylist': False})
        entries = []
        for e in info.get('entries') or []:
            if e:
                entries.append({k: e.get(k) for k in ENTRY_KEYS if e.get(k) is not None})
        return {'entries': entries}

    def _real_extract(self, url):
        ydl = self._downloader
        _reply({'ok': True, 'ready': True, 'pid': os.getpid()})
        stdin = sys.stdin.buffer
        while True:
            line = stdin.readline()
            if not line:
                sys.stderr.flush()
                os._exit(0)
            line = line.strip()
            if not line:
                continue
            try:
                req = json.loads(line)
                op = req.get('op')
                if op == 'resolve':
                    data = self._resolve(ydl, req['id'], req.get('clients') or ['visionos', 'tv_samsung', 'web_embedded', 'tv', 'default', 'visionos-page', 'web_safari'], req.get('visitor_data'))
                elif op == 'search':
                    n, start = int(req.get('n') or 24), int(req.get('start') or 0)
                    data = self._list(ydl, '%s%d:%s' % ('ytsearchdate' if req.get('newest') else 'ytsearch', start + n, req['q']), n, start)
                elif op == 'channel':
                    n, start = int(req.get('n') or 30), int(req.get('start') or 0)
                    data = self._list(ydl, 'https://www.youtube.com/channel/%s/videos' % req['id'], n, start)
                elif op == 'fetch':
                    data = self._fetch(ydl, req['url'])
                elif op == 'ping':
                    data = {'pong': True}
                else:
                    raise Exception('unknown op')
                _reply({'ok': True, 'data': data})
            except BaseException as e:  # keep serving
                if isinstance(e, (KeyboardInterrupt, SystemExit)):
                    os._exit(0)
                msg = str(e) or e.__class__.__name__
                sys.stderr.write('ERROR: %s\n' % msg)
                sys.stderr.flush()
                _reply({'ok': False, 'error': msg[:400]})
