#!/usr/bin/env python3
"""Тестовый сервер OPDS для проверки страницы «Каталоги» без интернета.

  python3 tests/net/opds_server.py [порт] [папка с книгами PDF]

  /opds            → переадресация на /opds/ (проверка final_url)
  /opds/…          → ленты из tests/net/opds/*.xml (root.xml для /opds/)
  /opds/search?q=  → результаты поиска (по названиям из new.xml)
  /secure/…        → то же, но нужен вход (логин reader, пароль pass)
  /dl/test.epub    → маленький EPUB (собирается на лету); ?slow=1 или OPDS_SLOW=1 — медленно
  /dl/<файл>.pdf   → файл из папки книг, с Content-Disposition (если имя не book*)
  /dl/anon         → PDF без расширения и без имени (определяется по содержимому)
  /img/*.png       → обложки (собираются на лету)
"""
import base64
import io
import os
import re
import sys
import time
import zipfile
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
FEEDS = os.path.join(HERE, "opds")
BOOKS = sys.argv[2] if len(sys.argv) > 2 else "/work/testdata"
AUTH = "Basic " + base64.b64encode(b"reader:pass").decode()


def make_epub():
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")  # без сжатия, первым
        z.writestr("META-INF/container.xml",
                   '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                   '<rootfiles><rootfile full-path="c.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
                   zipfile.ZIP_DEFLATED)
        z.writestr("c.opf",
                   '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="i">'
                   '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Тестовая книга</dc:title>'
                   '<dc:identifier id="i">urn:test</dc:identifier><dc:language>ru</dc:language></metadata>'
                   '<manifest><item id="a" href="a.xhtml" media-type="application/xhtml+xml"/></manifest>'
                   '<spine><itemref idref="a"/></spine></package>', zipfile.ZIP_DEFLATED)
        z.writestr("a.xhtml",
                   '<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head>'
                   '<body><h1>Тестовая книга</h1>' + "<p>Это проверочный текст книги. </p>" * 80 + '</body></html>',
                   zipfile.ZIP_DEFLATED)
    return b.getvalue()


def make_png(w, h, seed):
    rows = b""
    for y in range(h):
        rows += b"\0" + bytes(((x * 255 // w + y * 255 // h + seed * 23) // 2 % 256) for x in range(w))
    def chunk(t, d):
        return len(d).to_bytes(4, "big") + t + d + zlib.crc32(t + d).to_bytes(4, "big")
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big") + b"\x08\0\0\0\0")
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def feed_file(name):
    with open(os.path.join(FEEDS, name), "rb") as f:
        return f.read()


def search_feed(q):
    items = re.findall(r"<entry>.*?</entry>", feed_file("new.xml").decode() + feed_file("new2.xml").decode(), re.S)
    hit = [e for e in items if "<link rel=\"http://opds-spec.org/acquisition" in e and q.lower() in
           re.search(r"<title>(.*?)</title>", e).group(1).lower()]
    return ('<?xml version="1.0" encoding="utf-8"?>\n<feed xmlns="http://www.w3.org/2005/Atom">\n'
            "<id>urn:search</id><title>Поиск: %s</title>\n%s\n</feed>\n" % (q.replace("&", "&amp;"), "\n".join(hit))).encode()


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.command, self.path))

    def send(self, code, body, ctype, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        path = unquote(u.path)
        qs = parse_qs(u.query)
        if path.startswith("/secure/"):
            if self.headers.get("Authorization") != AUTH:
                return self.send(401, b"need login", "text/plain", [("WWW-Authenticate", 'Basic realm="test"')])
            path = path[len("/secure"):]
        if path == "/opds":
            return self.send(302, b"", "text/plain", [("Location", "/opds/")])
        if path == "/opds/":
            return self.send(200, feed_file("root.xml"), "application/atom+xml;profile=opds-catalog;kind=navigation")
        if path == "/opds/search":
            return self.send(200, search_feed(qs.get("q", [""])[0]), "application/atom+xml;profile=opds-catalog")
        if path == "/opds/osd.xml":
            return self.send(200, feed_file("osd.xml"), "application/opensearchdescription+xml")
        m = re.fullmatch(r"/opds/([a-z0-9]+)", path)
        if m and os.path.exists(os.path.join(FEEDS, m.group(1) + ".xml")):
            return self.send(200, feed_file(m.group(1) + ".xml"), "application/atom+xml;profile=opds-catalog")
        m = re.fullmatch(r"/img/(thumb|cover)(\d+)\.png", path)
        if m:
            big = m.group(1) == "cover"
            return self.send(200, make_png(300 if big else 90, 400 if big else 120, int(m.group(2))), "image/png")
        if path.startswith("/dl/"):
            return self.download(path[4:], "slow" in qs or bool(os.environ.get("OPDS_SLOW")))
        self.send(404, b"not found", "text/plain")

    def download(self, name, slow):
        extra = []
        if name == "test.epub":
            body, ctype = make_epub(), "application/epub+zip"
        elif name == "anon":
            body, ctype = open(os.path.join(BOOKS, "datasheet-2.pdf"), "rb").read(), "application/octet-stream"
        elif name.endswith(".pdf"):
            p = os.path.join(BOOKS, name)
            if not os.path.exists(p):
                return self.send(404, b"no file", "text/plain")
            body, ctype = open(p, "rb").read(), "application/pdf"
            extra = [("Content-Disposition", 'attachment; filename="%s"' % name)]
        else:
            return self.send(404, b"no file", "text/plain")
        if not slow:
            return self.send(200, body, ctype, extra)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        step = max(len(body) // 14, 1)
        for i in range(0, len(body), step):
            self.wfile.write(body[i:i + step])
            self.wfile.flush()
            time.sleep(0.35)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()
