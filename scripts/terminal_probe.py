"""Native Windows ConPTY input latency and server-observed delivery probe.

Only Python stdlib and the real server/daemon/client executables are used.
The terminal parser implements the cursor/erase subset emitted by ConPTY; raw VT
captures are retained so projection errors are inspectable, not hidden.
"""
from __future__ import annotations

import argparse
import codecs
import ctypes
from ctypes import wintypes as W
import hashlib
import html
import json
import math
import os
from pathlib import Path
import platform
import queue
import re
import statistics
import struct
import subprocess
import sys
import threading
import time
import unicodedata

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from journey import CREDENTIALS, Process, endpoint, require, send_message
from client_journey import status
from wire import Peer


class Background(Process):
    def __init__(self, argv, env, directory, name):
        self.log = (directory/f"{name}.stderr.log").open("w",encoding="utf-8")
        self.lines = queue.Queue()
        self.process = subprocess.Popen(argv,env=env,stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE,stderr=self.log,
                                        text=True,encoding="utf-8",errors="replace",
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.thread = threading.Thread(target=self._read,daemon=True)
        self.thread.start()


class Projection:
    """Small bounded screen projection, not a claim of complete terminal emulation."""
    def __init__(self, columns, rows):
        self.columns, self.rows = columns, rows
        self.cells = [[" "] * columns for _ in range(rows)]
        self.styles = [[("#d8dee9", "#15191f", False)] * columns for _ in range(rows)]
        self.fg, self.bg, self.bold = "#d8dee9", "#15191f", False
        self.reverse = False
        self.x = self.y = 0
        self.saved = (0, 0)
        self.pending = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.unknown = set()
        self.win32_input = False

    def feed(self, data):
        text = self.pending + self.decoder.decode(data)
        self.pending = ""
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                if i + 1 == len(text):
                    self.pending = text[i:]
                    break
                if text[i+1] == "[":
                    end = i + 2
                    while end < len(text) and not "@" <= text[end] <= "~":
                        end += 1
                    if end == len(text):
                        self.pending = text[i:]
                        break
                    self.csi(text[i+2:end], text[end])
                    i = end + 1
                    continue
                if text[i+1] in "]P_^":
                    end = re.search(r"\x07|\x1b\\", text[i+2:])
                    if not end:
                        self.pending = text[i:]
                        break
                    i += 2 + end.end()
                    continue
                if text[i+1] in "()":
                    if i+2 == len(text):
                        self.pending = text[i:]
                        break
                    i += 3
                    continue
                if text[i+1] == "7":
                    self.saved = self.x, self.y
                elif text[i+1] == "8":
                    self.x, self.y = self.saved
                i += 2
                continue
            if ch == "\r":
                self.x = 0
            elif ch in "\n\v\f":
                self.down()
            elif ch == "\b":
                self.x = max(0, self.x-1)
            elif ch == "\t":
                self.x = min(self.columns-1, (self.x//8+1)*8)
            elif ord(ch) >= 32:
                width = 0 if unicodedata.combining(ch) else (2 if unicodedata.east_asian_width(ch) in "WF" else 1)
                if width == 0:
                    self.cells[self.y][max(0, self.x-1)] += ch
                else:
                    if self.x >= self.columns:
                        self.x = 0
                        self.down()
                    self.cells[self.y][self.x] = ch
                    style = ((self.bg,self.fg,self.bold) if self.reverse else
                             (self.fg,self.bg,self.bold))
                    self.styles[self.y][self.x] = style
                    if width == 2 and self.x+1 < self.columns:
                        self.cells[self.y][self.x+1] = ""
                        self.styles[self.y][self.x+1] = style
                    self.x += width
            i += 1

    def down(self):
        self.y += 1
        if self.y == self.rows:
            self.cells.pop(0)
            self.cells.append([" "]*self.columns)
            self.styles.pop(0)
            self.styles.append([(self.fg,self.bg,self.bold)]*self.columns)
            self.y -= 1

    def csi(self, params, final):
        raw = params.lstrip("?<=>!")
        values = [int(v) if v.isdigit() else 0 for v in raw.split(";")]
        n = values[0] or 1
        if final in "Hf":
            self.y = max(0, min(self.rows-1, n-1))
            self.x = max(0, min(self.columns-1, (values[1] if len(values)>1 and values[1] else 1)-1))
        elif final == "A": self.y = max(0, self.y-n)
        elif final == "B": self.y = min(self.rows-1, self.y+n)
        elif final == "C": self.x = min(self.columns-1, self.x+n)
        elif final == "D": self.x = max(0, self.x-n)
        elif final in "G`": self.x = max(0, min(self.columns-1, n-1))
        elif final == "d": self.y = max(0, min(self.rows-1, n-1))
        elif final == "E": self.y, self.x = min(self.rows-1, self.y+n), 0
        elif final == "F": self.y, self.x = max(0, self.y-n), 0
        elif final == "J":
            mode = values[0]
            for y in range(self.rows):
                if mode in (2,3) or (mode == 0 and y > self.y) or (mode == 1 and y < self.y):
                    self.cells[y] = [" "]*self.columns
                    self.styles[y] = [(self.fg,self.bg,self.bold)]*self.columns
            if mode in (0,1):
                a,b = (self.x,self.columns) if mode == 0 else (0,self.x+1)
                self.cells[self.y][a:b] = [" "]*(b-a)
                self.styles[self.y][a:b] = [(self.fg,self.bg,self.bold)]*(b-a)
        elif final == "K":
            mode = values[0]
            a,b = (self.x,self.columns) if mode == 0 else ((0,self.x+1) if mode == 1 else (0,self.columns))
            self.cells[self.y][a:b] = [" "]*(b-a)
            self.styles[self.y][a:b] = [(self.fg,self.bg,self.bold)]*(b-a)
        elif final == "X":
            end = min(self.columns,self.x+n)
            self.cells[self.y][self.x:end] = [" "]*(end-self.x)
            self.styles[self.y][self.x:end] = [(self.fg,self.bg,self.bold)]*(end-self.x)
        elif final in "ST":
            for _ in range(min(n,self.rows)):
                if final == "S":
                    self.cells.pop(0)
                    self.styles.pop(0)
                    self.cells.append([" "]*self.columns)
                    self.styles.append([(self.fg,self.bg,self.bold)]*self.columns)
                else:
                    self.cells.pop()
                    self.styles.pop()
                    self.cells.insert(0,[" "]*self.columns)
                    self.styles.insert(0,[(self.fg,self.bg,self.bold)]*self.columns)
        elif final == "P":
            n = min(n,self.columns-self.x)
            del self.cells[self.y][self.x:self.x+n]
            del self.styles[self.y][self.x:self.x+n]
            self.cells[self.y].extend([" "]*n)
            self.styles[self.y].extend([(self.fg,self.bg,self.bold)]*n)
        elif final == "m": self.sgr(values)
        elif final == "s": self.saved = self.x,self.y
        elif final == "u": self.x,self.y = self.saved
        elif final in "hl":
            if params == "?9001":
                self.win32_input = final == "h"
        elif final in "ntcq": pass
        else: self.unknown.add(params+final)

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.cells)

    def sgr(self, values):
        palette = ["#15191f","#e06c75","#98c379","#e5c07b","#61afef","#c678dd","#56b6c2","#d8dee9",
                   "#5c6370","#ff7b86","#b7e68a","#ffda8a","#83c8ff","#e199f0","#79dae5","#ffffff"]
        index = 0
        while index < len(values):
            value = values[index]
            if value == 0:
                self.fg,self.bg,self.bold = "#d8dee9","#15191f",False
                self.reverse = False
            elif value == 1: self.bold = True
            elif value == 22: self.bold = False
            elif value == 7: self.reverse = True
            elif value == 27: self.reverse = False
            elif value == 39: self.fg = "#d8dee9"
            elif value == 49: self.bg = "#15191f"
            elif 30 <= value <= 37: self.fg = palette[value-30]
            elif 90 <= value <= 97: self.fg = palette[value-90+8]
            elif 40 <= value <= 47: self.bg = palette[value-40]
            elif 100 <= value <= 107: self.bg = palette[value-100+8]
            elif value in (38,48) and index+2 < len(values):
                color = None
                if values[index+1] == 2 and index+4 < len(values):
                    color = "#"+"".join(f"{v:02x}" for v in values[index+2:index+5])
                    index += 4
                elif values[index+1] == 5:
                    n = values[index+2]
                    if n < 16: color = palette[n]
                    elif n < 232:
                        n -= 16
                        levels = (0,95,135,175,215,255)
                        color = "#"+"".join(f"{levels[v]:02x}" for v in (n//36,(n//6)%6,n%6))
                    else: color = "#"+f"{8+10*(n-232):02x}"*3
                    index += 2
                if color:
                    if value == 38: self.fg = color
                    else: self.bg = color
            index += 1

    def svg(self):
        cw,ch,pad = 10,22,16
        parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.columns*cw+pad*2}" height="{self.rows*ch+pad*2}" viewBox="0 0 {self.columns*cw+pad*2} {self.rows*ch+pad*2}">',
                 '<rect width="100%" height="100%" fill="#15191f"/>',
                 '<g font-family="Consolas, Cascadia Mono, Microsoft YaHei UI, monospace" font-size="17">']
        for y,row in enumerate(self.cells):
            for x,value in enumerate(row):
                fg,bg,bold = self.styles[y][x]
                px,py = pad+x*cw,pad+y*ch
                if bg != "#15191f":
                    parts.append(f'<rect x="{px}" y="{py}" width="{cw}" height="{ch}" fill="{bg}"/>')
        # Paint backgrounds first: a wide glyph crosses into the following
        # cell, whose background must not subsequently cover half the glyph.
        for y,row in enumerate(self.cells):
            for x,value in enumerate(row):
                fg,bg,bold = self.styles[y][x]
                px,py = pad+x*cw,pad+y*ch
                if value.strip():
                    parts.append(f'<text x="{px}" y="{py+17}" fill="{fg}" font-weight="{700 if bold else 400}">{html.escape(value)}</text>')
        parts.append('</g></svg>')
        return "".join(parts)


class ConPTY:
    def __init__(self, argv, columns=140, rows=36, env=None):
        if os.name != "nt":
            raise RuntimeError("ConPTY probe requires Windows")
        class COORD(ctypes.Structure):
            _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]
        class STARTUPINFO(ctypes.Structure):
            _fields_ = [("cb", W.DWORD), ("lpReserved", W.LPWSTR), ("lpDesktop", W.LPWSTR),
                        ("lpTitle", W.LPWSTR), ("dwX", W.DWORD), ("dwY", W.DWORD),
                        ("dwXSize", W.DWORD), ("dwYSize", W.DWORD), ("dwXCountChars", W.DWORD),
                        ("dwYCountChars", W.DWORD), ("dwFillAttribute", W.DWORD),
                        ("dwFlags", W.DWORD), ("wShowWindow", W.WORD), ("cbReserved2", W.WORD),
                        ("lpReserved2", ctypes.POINTER(W.BYTE)), ("hStdInput", W.HANDLE),
                        ("hStdOutput", W.HANDLE), ("hStdError", W.HANDLE)]
        class STARTUPINFOEX(ctypes.Structure):
            _fields_ = [("StartupInfo", STARTUPINFO), ("lpAttributeList", ctypes.c_void_p)]
        class PROCESS_INFORMATION(ctypes.Structure):
            _fields_ = [("hProcess", W.HANDLE), ("hThread", W.HANDLE),
                        ("dwProcessId", W.DWORD), ("dwThreadId", W.DWORD)]
        self.k = k = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "CreatePipe": ([ctypes.POINTER(W.HANDLE),ctypes.POINTER(W.HANDLE),ctypes.c_void_p,W.DWORD],W.BOOL),
            "CreatePseudoConsole": ([COORD,W.HANDLE,W.HANDLE,W.DWORD,ctypes.POINTER(W.HANDLE)],ctypes.c_long),
            "ClosePseudoConsole": ([W.HANDLE],None),
            "CloseHandle": ([W.HANDLE],W.BOOL),
            "InitializeProcThreadAttributeList": ([ctypes.c_void_p,W.DWORD,W.DWORD,ctypes.POINTER(ctypes.c_size_t)],W.BOOL),
            "UpdateProcThreadAttribute": ([ctypes.c_void_p,W.DWORD,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p],W.BOOL),
            "DeleteProcThreadAttributeList": ([ctypes.c_void_p],None),
            "CreateProcessW": ([W.LPCWSTR,W.LPWSTR,ctypes.c_void_p,ctypes.c_void_p,W.BOOL,W.DWORD,ctypes.c_void_p,W.LPCWSTR,ctypes.c_void_p,ctypes.POINTER(PROCESS_INFORMATION)],W.BOOL),
            "ReadFile": ([W.HANDLE,ctypes.c_void_p,W.DWORD,ctypes.POINTER(W.DWORD),ctypes.c_void_p],W.BOOL),
            "WriteFile": ([W.HANDLE,ctypes.c_void_p,W.DWORD,ctypes.POINTER(W.DWORD),ctypes.c_void_p],W.BOOL),
            "WaitForSingleObject": ([W.HANDLE,W.DWORD],W.DWORD),
            "GetExitCodeProcess": ([W.HANDLE,ctypes.POINTER(W.DWORD)],W.BOOL),
            "TerminateProcess": ([W.HANDLE,W.UINT],W.BOOL),
            "GetStdHandle": ([W.DWORD],W.HANDLE),
            "SetStdHandle": ([W.DWORD,W.HANDLE],W.BOOL),
        }
        for name,(args,result) in signatures.items():
            getattr(k,name).argtypes, getattr(k,name).restype = args,result
        inp,outp = W.HANDLE(),W.HANDLE()
        self.input,self.output,self.console = W.HANDLE(),W.HANDLE(),W.HANDLE()
        self.check(k.CreatePipe(ctypes.byref(inp),ctypes.byref(self.input),None,0))
        self.check(k.CreatePipe(ctypes.byref(self.output),ctypes.byref(outp),None,0))
        hr = k.CreatePseudoConsole(COORD(columns,rows),inp,outp,0,ctypes.byref(self.console))
        require(hr == 0, f"CreatePseudoConsole HRESULT {hr:#x}")
        size = ctypes.c_size_t()
        k.InitializeProcThreadAttributeList(None,1,0,ctypes.byref(size))
        attrs = ctypes.create_string_buffer(size.value)
        self.check(k.InitializeProcThreadAttributeList(attrs,1,0,ctypes.byref(size)))
        self.check(k.UpdateProcThreadAttribute(attrs,0,0x20016,self.console,ctypes.sizeof(self.console),None,None))
        info = STARTUPINFOEX()
        info.StartupInfo.cb = ctypes.sizeof(info)
        info.lpAttributeList = ctypes.cast(attrs,ctypes.c_void_p)
        process = PROCESS_INFORMATION()
        command = ctypes.create_unicode_buffer(subprocess.list2cmdline(argv))
        environment = (ctypes.create_unicode_buffer("\0".join(
            f"{key}={value}" for key,value in sorted(env.items(),key=lambda item:item[0].upper()))+"\0\0")
            if env is not None else None)
        # Tool/CI hosts redirect std handles. Without clearing these, Windows
        # passes host pipes through rather than installing the ConPTY handles.
        std_ids = (0xfffffff6, 0xfffffff5, 0xfffffff4)
        old_std = [k.GetStdHandle(i) for i in std_ids]
        for i in std_ids:
            k.SetStdHandle(i, None)
        try:
            self.check(k.CreateProcessW(None,command,None,None,False,0x80000|0x400,environment,str(ROOT),ctypes.byref(info),ctypes.byref(process)))
        finally:
            for i, handle in zip(std_ids, old_std):
                k.SetStdHandle(i, handle)
            k.DeleteProcThreadAttributeList(attrs)
            k.CloseHandle(inp)
            k.CloseHandle(outp)
        k.CloseHandle(process.hThread)
        self.process,self.pid = process.hProcess,process.dwProcessId
        self.projection = Projection(columns,rows)
        self.raw = bytearray()
        self.samples = []
        self.condition = threading.Condition()
        self.thread = threading.Thread(target=self._read,daemon=True)
        self.thread.start()

    @staticmethod
    def check(value):
        if not value: raise ctypes.WinError(ctypes.get_last_error())

    def _read(self):
        buf = ctypes.create_string_buffer(65536)
        count = W.DWORD()
        while self.k.ReadFile(self.output,buf,len(buf),ctypes.byref(count),None) and count.value:
            stamp = time.perf_counter()
            data = buf.raw[:count.value]
            with self.condition:
                self.raw.extend(data)
                self.projection.feed(data)
                self.samples.append((stamp,len(self.raw)))
                self.condition.notify_all()

    def write(self, text):
        if isinstance(text,str) and self.projection.win32_input:
            # Honor ConPTY's negotiated Win32-input mode, as Windows Terminal
            # does. Sending plain UTF-8 after ?9001h loses native Unicode keys.
            keys = {"\x1b[D":37,"\x1b[C":39,"\x1b[A":38,"\x1b[B":40,
                    "\x1b[H":36,"\x1b[F":35,"\x1b[3~":46}
            events = []
            while text:
                matched = next((key for key in keys if text.startswith(key)),None)
                if matched:
                    events.append(f"\x1b[{keys[matched]};0;0;1;0;1_")
                    events.append(f"\x1b[{keys[matched]};0;0;0;0;1_")
                    text = text[len(matched):]
                    continue
                ch,text = text[0],text[1:]
                units = ch.encode("utf-16-le")
                for (unit,) in struct.iter_unpack("<H",units):
                    vk = ord(ch.upper()) if ch.isascii() and ch.isalnum() else (231 if unit > 127 else 0)
                    controls = 0
                    if unit == 13: vk = 13
                    elif unit == 3: vk,controls = 67,8
                    events.append(f"\x1b[{vk};0;{unit};1;{controls};1_")
                    events.append(f"\x1b[{vk};0;{unit};0;{controls};1_")
            data = "".join(events).encode("ascii")
        else:
            data = text.encode("utf-8") if isinstance(text,str) else text
        count = W.DWORD()
        stamp = time.perf_counter()
        self.check(self.k.WriteFile(self.input,data,len(data),ctypes.byref(count),None))
        require(count.value == len(data), "short ConPTY input write")
        return stamp

    def wait(self, needle, timeout=15):
        deadline = time.perf_counter()+timeout
        with self.condition:
            while needle not in self.projection.text():
                left = deadline-time.perf_counter()
                if left <= 0:
                    raise AssertionError(f"screen missing {needle!r}:\n{self.projection.text()}")
                self.condition.wait(left)
            return self.samples[-1][0]

    def save(self, directory, name):
        with self.condition:
            (directory/f"{name}.vt").write_bytes(self.raw)
            (directory/f"{name}.screen.txt").write_text(self.projection.text(),encoding="utf-8")
            (directory/f"{name}.svg").write_text(self.projection.svg(),encoding="utf-8")
            return {"bytes":len(self.raw),"unknown_csi":sorted(self.projection.unknown)}

    def finish(self):
        require(self.k.WaitForSingleObject(self.process,8000) == 0,"TUI did not exit")
        code = W.DWORD()
        self.check(self.k.GetExitCodeProcess(self.process,ctypes.byref(code)))
        require(code.value == 0,f"TUI failed with {code.value}")

    def close(self):
        if self.k.WaitForSingleObject(self.process,0) != 0:
            self.k.TerminateProcess(self.process,1)
            self.k.WaitForSingleObject(self.process,5000)
        self.k.ClosePseudoConsole(self.console)
        self.thread.join(timeout=5)
        self.k.CloseHandle(self.input)
        self.k.CloseHandle(self.output)
        self.k.CloseHandle(self.process)


def percentile(values, fraction):
    return sorted(values)[min(len(values)-1,math.ceil(len(values)*fraction)-1)]


def run(args):
    work = Path(args.output).resolve()
    require(work.is_relative_to(ROOT/".cache") or work.is_relative_to(ROOT/".temp"),"output must stay in .cache/.temp")
    work.mkdir(parents=True,exist_ok=True)
    env = dict(os.environ,CHAT_CREDENTIALS=CREDENTIALS,CHAT_TOKEN="alice-secret",TERM="xterm-256color")
    # This tool host sets NO_COLOR=1 and TERM=dumb. A real terminal fixture must
    # advertise its capabilities rather than inherit the tool log preferences.
    env.pop("NO_COLOR",None)
    env["COLORTERM"] = "truecolor"
    processes = []
    ui = None
    result = {"platform":platform.platform(),"processor":platform.processor(),
              "logical_cpus":os.cpu_count(),"python":sys.version,
              "client_sha256":hashlib.sha256(Path(args.client).read_bytes()).hexdigest(),
              "server_sha256":hashlib.sha256(Path(args.server).read_bytes()).hexdigest(),
              "daemon_sha256":hashlib.sha256(Path(args.daemon).read_bytes()).hexdigest(),
              "harness_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "columns":args.columns,"rows":args.rows,"rounds":args.rounds,"samples":[]}
    try:
        server = Background([args.server,"--memory","--port","0"],env,work,"server")
        processes.append(server)
        port = server.ready()
        state = work/f"alice-{time.time_ns()}"
        daemon = Background([args.daemon,"--port",str(port),"--tenant","alpha","--user","alice","--state-dir",str(state)],env,work,"daemon")
        processes.append(daemon)
        ipc = daemon.ready()
        _,secret = endpoint(state)
        with Peer(ipc) as observer,Peer(port) as bob:
            observer.hello("alpha","alice",secret)
            status(observer,"online")
            bob.hello("alpha","bob","bob-secret")
            if args.showcase:
                send_message(bob,"现在输入终于跟手了，中文和 emoji 也可以一起聊。🌸")
                send_message(observer,"收到！消息、时间和已读状态现在一眼就能看清。")
                send_message(bob,"我把性能记录放到研究室了。这里继续聊产品设计吧。")
                final_id,_ = send_message(observer,"好呀。界面应该把空间留给对话，而不是调试日志。")
                bob.send(7,["lobby",final_id])
                bob.wait(8)
                join = observer.send(14,["research"])
                observer.wait(15,join)
                send_message(bob,"完整测试结果已归档，稍后一起看。","research")
                ui = ConPTY([args.client,"--state-dir",str(state)],args.columns,args.rows,env)
                ui.wait("日志。",timeout=15)
                ui.write("下一步一起试试多房间？🌙")
                ui.wait("多房间")
                result["capture"] = ui.save(work,"conversation")
                ui.write("\x03")
                ui.finish()
                require(len(bob.history()) == 4,"closing showcase UI unexpectedly sent the unsent draft")
                counts = observer.rooms()
                require(counts["lobby"][1] == 2 and counts["research"][1] == 1,
                        "showcase viewing implicitly changed unread counts")
                result["unread_after_view"] = {"lobby":2,"research":1}
                result["lobby_messages_after_exit"] = 4
                result["status"] = "showcase_passed"
                result["clean_exit"] = True
                return
            if args.stress_history:
                seed = [f"历史消息 {i}: 中文与emoji 🌸，检查历史不会拖慢输入。" for i in range(95)]
                seed += [f"长历史 {i}: "+"历史内容"*5300+"末尾🌸" for i in range(6)]
                seed += ["历史完成标记：现在输入依然应该跟手。"]
                for index,body in enumerate(seed):
                    request = bob.send(3,["lobby",f"stress-{index}",body])
                    bob.wait(15,request)
                    require(bob.wait(4).fields[4] == body,"history seed changed before UI startup")
                result["stress_history"] = {"messages":len(seed),
                    "large_messages":6,"largest_utf8_bytes":max(len(s.encode()) for s in seed)}
            ui = ConPTY([args.client,"--state-dir",str(state)],args.columns,args.rows,env)
            # Both baseline and new UI have the room name after successful attach.
            ui.wait("lobby")
            if args.stress_history:
                ui.wait("历史完成标记",timeout=25)
            time.sleep(.3)
            latencies = []
            verified = 0
            for round_index in range(args.rounds):
                text = f"K{round_index}-"
                ui.write(text)
                ui.wait(text)
                for ch in "QWERTYUIOPASDFGHJKLZXCVBNM":
                    text += ch
                    sent = ui.write(ch)
                    seen = ui.wait(text)
                    latencies.append((seen-sent)*1000)
                ui.write("\r")
                event = bob.wait(4,timeout=15)
                require(event.fields[4] == text,"typed ASCII differs at independent server peer")
                verified += 1
                result["samples"].append({"kind":"single_keys","round":round_index,"count":len(text)})
                time.sleep(.08)
            result["input_to_visible_ms"] = {"n":len(latencies),"p50":statistics.median(latencies),"p95":percentile(latencies,.95),"max":max(latencies),"values":latencies}
            for name,body in (("ascii_burst","ASCII-"+"abcdefghijklmnop"*6+"-END"),("unicode_burst","中文输入不卡顿🌸🙂"*8+"完毕")):
                timings = []
                for round_index in range(args.rounds):
                    text = body + f"-{round_index}"
                    sent = ui.write(text)
                    try:
                        seen = ui.wait(text[-6:], timeout=10)
                    except AssertionError as error:
                        if not args.allow_failures:
                            raise
                        ui.write("\r")
                        try:
                            event = bob.wait(4,timeout=3)
                            result[name] = {"status":"payload_mismatch", "expected":text,"received":event.fields[4]}
                        except TimeoutError:
                            result[name] = {"status":"stalled", "deadline_ms":10000,"expected":text,"screen":ui.projection.text()}
                        result["status"] = "baseline_failure_reproduced"
                        result["capture"] = ui.save(work,"conversation")
                        print(json.dumps(result,ensure_ascii=False,indent=2))
                        return
                    timings.append((seen-sent)*1000)
                    ui.write("\r")
                    event = bob.wait(4,timeout=15)
                    require(event.fields[4] == text,
                            f"{name} changed at independent server peer: expected {text!r}, received {event.fields[4]!r}")
                    verified += 1
                    time.sleep(.08)
                result[name] = {"codepoints":len(text),"utf8_bytes":len(text.encode()),"completion_ms":timings,"p50":statistics.median(timings),"p95":percentile(timings,.95)}
                if args.max_burst_ms:
                    require(max(timings) <= args.max_burst_ms,
                            f"{name} exceeded {args.max_burst_ms} ms budget: {timings}")
            unicode_latencies = []
            for round_index in range(args.rounds):
                text = f"U{round_index}-"
                ui.write(text)
                ui.wait(text)
                for ch in "你好世界🌸🙂中文":
                    text += ch
                    sent = ui.write(ch)
                    try:
                        seen = ui.wait(text,timeout=3)
                    except AssertionError:
                        if not args.allow_failures:
                            raise
                        result["unicode_input_to_visible_ms"] = {
                            "status":"stalled","deadline_ms":3000,"draft_expected":text,
                            "completed_values":unicode_latencies,"screen":ui.projection.text()}
                        result["status"] = "baseline_failure_reproduced"
                        result["server_observed_payloads"] = verified
                        result["capture"] = ui.save(work,"conversation")
                        return
                    unicode_latencies.append((seen-sent)*1000)
                ui.write("\r")
                require(bob.wait(4).fields[4] == text,"single Unicode keys changed at server peer")
                verified += 1
                time.sleep(.08)
            result["unicode_input_to_visible_ms"] = {
                "n":len(unicode_latencies),"p50":statistics.median(unicode_latencies),
                "p95":percentile(unicode_latencies,.95),"max":max(unicode_latencies),
                "values":unicode_latencies}
            if args.editing:
                ui.write("原始🙂ABC")
                ui.wait("原始🙂ABC")
                ui.write("\x1b[D\x1b[D\x1b[3~")
                ui.wait("原始🙂AC")
                ui.write("\x1b[H前\x1b[F后\r")
                expected = "前原始🙂AC后"
                require(bob.wait(4).fields[4] == expected,"native cursor/home/end/delete edit changed payload")
                verified += 1
                result["editing"] = "passed"
            # A real remote message for a useful visual capture, not only an empty composer.
            bob.send(3,["lobby","visual-peer","这次是真的跨进程收发。输入应当跟手，消息应当好读。🌸"])
            bob.wait(15)
            ui.wait("输入应当")
            ui.write("正在写下一条消息 🌙")
            ui.wait("下一条消息")
            result["capture"] = ui.save(work,"conversation")
            # Do not accidentally publish the unsent visual draft.
            ui.write("\x03")
            ui.finish()
            result["clean_exit"] = True
            result["server_observed_payloads"] = verified
            result["status"] = "passed"
    finally:
        if ui:
            ui.save(work,"final")
            ui.close()
        for process in reversed(processes): process.close()
        (work/"result.json").write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding="utf-8")
    print(json.dumps(result,ensure_ascii=False,indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server","daemon","client"):
        parser.add_argument("--"+name)
    parser.add_argument("--output",required=True)
    parser.add_argument("--fixture",help="run a native console fixture rather than the chat workload")
    parser.add_argument("--rounds",type=int,default=3)
    parser.add_argument("--columns",type=int,default=140)
    parser.add_argument("--rows",type=int,default=36)
    parser.add_argument("--max-burst-ms",type=float,default=0,
                        help="optional per-burst latency gate (0 records measurements only)")
    parser.add_argument("--allow-failures",action="store_true",help="retain reproducible old-client failures as baseline evidence")
    parser.add_argument("--editing",action="store_true",help="also require native cursor/home/end/delete editing")
    parser.add_argument("--stress-history",action="store_true",help="seed 102 messages including six ~64 KiB CJK bodies before measuring")
    parser.add_argument("--showcase",action="store_true",help="capture a genuine short conversation instead of a timing workload")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("the native ConPTY journey requires Windows")
    if args.fixture:
        work = Path(args.output).resolve()
        require(work.is_relative_to(ROOT/".cache") or work.is_relative_to(ROOT/".temp"),
                "output must stay in .cache/.temp")
        work.mkdir(parents=True,exist_ok=True)
        ui = ConPTY([str(Path(args.fixture).resolve()),str(work/"fixture-result.txt")],args.columns,args.rows)
        try:
            ui.finish()
            print((work/"fixture-result.txt").read_text(encoding="utf-8"))
        finally:
            ui.save(work,"fixture")
            ui.close()
        return
    for name in ("server","daemon","client"):
        if not getattr(args,name):
            parser.error("--server, --daemon, and --client are required without --fixture")
        setattr(args,name,str(Path(getattr(args,name)).resolve()))
    run(args)


if __name__ == "__main__":
    main()
