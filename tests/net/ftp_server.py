#!/usr/bin/env python3
"""Маленький FTP-сервер для проверки sync_run (в образе нет FTP-сервера, а rclone 1.53 не умеет serve ftp).
   ftp_server.py ПАПКА ПОРТ ЛОГИН ПАРОЛЬ [mlsd|list]
   Умеет то, что просит libcurl: USER PASS PWD CWD TYPE SIZE MDTM EPSV PASV LIST MLSD RETR STOR MKD QUIT.
   Режим list — без MLSD (как старые серверы)."""
import os
import socket
import socketserver
import sys
import time

ROOT = os.path.realpath(sys.argv[1])
PORT = int(sys.argv[2])
USER, PASS = sys.argv[3], sys.argv[4]
MLSD = (sys.argv[5] if len(sys.argv) > 5 else "mlsd") == "mlsd"


class Handler(socketserver.StreamRequestHandler):
    def reply(self, text):
        self.wfile.write((text + "\r\n").encode("utf-8"))
        self.wfile.flush()

    def real(self, name):
        p = os.path.normpath(os.path.join(ROOT, self.cwd.lstrip("/"), name.lstrip("/") if name.startswith("/") else name))
        if name.startswith("/"):
            p = os.path.normpath(os.path.join(ROOT, name.lstrip("/")))
        if not (p == ROOT or p.startswith(ROOT + os.sep)):
            return None
        return p

    def data_conn(self):
        self.reply("150 Opening data connection")
        self.pasv.settimeout(10)
        conn, _ = self.pasv.accept()
        self.pasv.close()
        self.pasv = None
        return conn

    def listing(self, path, mlsd):
        lines = []
        for n in sorted(os.listdir(path)):
            st = os.stat(os.path.join(path, n))
            isdir = os.path.isdir(os.path.join(path, n))
            if mlsd:
                lines.append("type=%s;size=%d;modify=%s; %s" % (
                    "dir" if isdir else "file", st.st_size,
                    time.strftime("%Y%m%d%H%M%S", time.gmtime(st.st_mtime)), n))
            else:
                lines.append("%s 1 u u %12d %s %s" % (
                    "drwxr-xr-x" if isdir else "-rw-r--r--", st.st_size,
                    time.strftime("%b %d %H:%M", time.gmtime(st.st_mtime)), n))
        return ("\r\n".join(lines) + "\r\n").encode("utf-8") if lines else b""

    def handle(self):
        self.cwd = "/"
        self.pasv = None
        self.ok = False
        self.user = None
        self.reply("220 test ftp")
        while True:
            line = self.rfile.readline()
            if not line:
                return
            line = line.decode("utf-8", "replace").rstrip("\r\n")
            cmd, _, arg = line.partition(" ")
            cmd = cmd.upper()
            if cmd == "USER":
                self.user = arg
                self.reply("331 Password required")
            elif cmd == "PASS":
                self.ok = self.user == USER and arg == PASS
                self.reply("230 Logged in" if self.ok else "530 Login incorrect")
            elif not self.ok and cmd != "QUIT":
                self.reply("530 Not logged in")
            elif cmd == "QUIT":
                self.reply("221 Bye")
                return
            elif cmd in ("TYPE", "NOOP", "MODE", "STRU"):
                self.reply("200 OK")
            elif cmd == "PWD":
                self.reply('257 "%s"' % self.cwd)
            elif cmd == "CWD":
                p = self.real(arg)
                if p and os.path.isdir(p):
                    self.cwd = "/" + os.path.relpath(p, ROOT).replace(".", "", 1) if p != ROOT else "/"
                    if p != ROOT:
                        self.cwd = "/" + os.path.relpath(p, ROOT)
                    self.reply("250 OK")
                else:
                    self.reply("550 No such directory")
            elif cmd in ("EPSV", "PASV"):
                self.pasv = socket.socket()
                self.pasv.bind(("127.0.0.1", 0))
                self.pasv.listen(1)
                port = self.pasv.getsockname()[1]
                if cmd == "EPSV":
                    self.reply("229 Entering Extended Passive Mode (|||%d|)" % port)
                else:
                    self.reply("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (port >> 8, port & 255))
            elif cmd == "SIZE":
                p = self.real(arg)
                if p and os.path.isfile(p):
                    self.reply("213 %d" % os.path.getsize(p))
                else:
                    self.reply("550 No such file")
            elif cmd == "MDTM":
                p = self.real(arg)
                if p and os.path.isfile(p):
                    self.reply("213 " + time.strftime("%Y%m%d%H%M%S", time.gmtime(os.path.getmtime(p))))
                else:
                    self.reply("550 No such file")
            elif cmd in ("LIST", "NLST", "MLSD"):
                if cmd == "MLSD" and not MLSD:
                    self.reply("500 Unknown command")
                    continue
                p = self.real(arg if arg and not arg.startswith("-") else "")
                if not p or not os.path.isdir(p) or not self.pasv:
                    self.reply("550 Failed")
                    continue
                conn = self.data_conn()
                conn.sendall(self.listing(p, cmd == "MLSD"))
                conn.close()
                self.reply("226 Done")
            elif cmd == "RETR":
                p = self.real(arg)
                if not p or not os.path.isfile(p) or not self.pasv:
                    self.reply("550 No such file")
                    continue
                conn = self.data_conn()
                with open(p, "rb") as f:
                    while True:
                        b = f.read(65536)
                        if not b:
                            break
                        try:
                            conn.sendall(b)
                        except OSError:
                            break
                conn.close()
                self.reply("226 Done")
            elif cmd == "STOR":
                p = self.real(arg)
                if not p or not os.path.isdir(os.path.dirname(p)) or not self.pasv:
                    self.reply("550 Cannot store")
                    continue
                conn = self.data_conn()
                with open(p, "wb") as f:
                    while True:
                        b = conn.recv(65536)
                        if not b:
                            break
                        f.write(b)
                conn.close()
                self.reply("226 Done")
            elif cmd == "MKD":
                p = self.real(arg)
                try:
                    os.mkdir(p)
                    self.reply('257 "%s" created' % arg)
                except OSError:
                    self.reply("550 Cannot create")
            else:
                self.reply("502 Not implemented")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


Server(("127.0.0.1", PORT), Handler).serve_forever()
