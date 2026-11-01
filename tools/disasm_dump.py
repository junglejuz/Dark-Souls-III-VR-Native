"""Disassemble shader bytecode dumped by SHADERDUMP (settings shader_dump=1 -> <game>\DS3VR2\shaders). Usage:
python -I disasm_dump.py <out_dir> <hash>-ps.bin ...   (uses d3dcompiler_47.dll D3DDisassemble)"""
import ctypes, sys, os
d=ctypes.WinDLL("d3dcompiler_47.dll")
class Blob(ctypes.Structure): pass
src=r"E:\Games\Steam\steamapps\common\DARK SOULS III\Game\DS3VR2\shaders"
out=sys.argv[1]
for name in sys.argv[2:]:
    b=open(os.path.join(src,name),"rb").read()
    blob=ctypes.c_void_p()
    hr=d.D3DDisassemble(ctypes.c_char_p(b), ctypes.c_size_t(len(b)), 0, None, ctypes.byref(blob))
    vt=ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    GetPtr=ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3]); GetSize=ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    txt=ctypes.string_at(GetPtr(blob), GetSize(blob)).decode("latin1")
    open(os.path.join(out,name+".txt"),"w").write(txt)
    print(name, hr, len(txt))
