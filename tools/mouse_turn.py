"""Relative mouse movement sent to the Dark Souls III window (focuses it first). Usage: mouse_turn.py <steps> <dx> <dy>
180 deg turn = 180 100 0 (one quick turn); small tilt up = 10 0 -12. Only when JJ allows camera control."""
import ctypes, time, importlib.util, sys
spec=importlib.util.spec_from_file_location("d", r"D:\Documents\Antigravity\VR_Modding\ds3vr2\tools\ds_drive.py"); d=importlib.util.module_from_spec(spec); spec.loader.exec_module(d)
h=d.game_window(); d.focus(h)
class MI(ctypes.Structure): _fields_=[("dx",ctypes.c_long),("dy",ctypes.c_long),("mouseData",ctypes.c_ulong),("dwFlags",ctypes.c_ulong),("time",ctypes.c_ulong),("dwExtraInfo",ctypes.c_size_t)]
class U(ctypes.Union): _fields_=[("mi",MI),("pad",ctypes.c_byte*32)]
class IN(ctypes.Structure): _fields_=[("type",ctypes.c_ulong),("u",U)]
def move(dx,dy):
    i=IN(); i.type=0; i.u.mi.dx=dx; i.u.mi.dy=dy; i.u.mi.dwFlags=0x0001
    ctypes.windll.user32.SendInput(1,ctypes.byref(i),ctypes.sizeof(IN))
n=int(sys.argv[1]); dx=int(sys.argv[2]); dy=int(sys.argv[3])
for _ in range(n): move(dx,dy); time.sleep(0.01)
