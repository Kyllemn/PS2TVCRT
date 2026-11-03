"""Frontend libretro minimo (ctypes) para testes automaticos: roda frames, botoes por script, RAM, capturas."""
import ctypes as C, os
from PIL import Image
CORE='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/cores/pcsx_rearmed/pcsx_rearmed_libretro.so'
SYS=b'/root/fm1/emu/system'
class GameInfo(C.Structure): _fields_=[('path',C.c_char_p),('data',C.c_void_p),('size',C.c_size_t),('meta',C.c_char_p)]
class Var(C.Structure): _fields_=[('key',C.c_char_p),('value',C.c_char_p)]
B={'B':0,'Y':1,'SELECT':2,'START':3,'UP':4,'DOWN':5,'LEFT':6,'RIGHT':7,'A':8,'X':9,'L':10,'R':11,'L2':12,'R2':13}
# PS1 no libretro: CRUZ=B, CIRCULO=A, QUADRADO=Y, TRIANGULO=X
class Emu:
    def __init__(self, game, opts=None):
        self.L=C.CDLL(CORE); L=self.L
        self.opts=dict(opts or {}); self.pressed=set(); self.frame=None; self.fmt=0
        self._keep=[]
        ENV=C.CFUNCTYPE(C.c_bool,C.c_uint,C.c_void_p)
        VID=C.CFUNCTYPE(None,C.c_void_p,C.c_uint,C.c_uint,C.c_size_t)
        AUD=C.CFUNCTYPE(None,C.c_int16,C.c_int16)
        AUDB=C.CFUNCTYPE(C.c_size_t,C.c_void_p,C.c_size_t)
        POLL=C.CFUNCTYPE(None)
        STATE=C.CFUNCTYPE(C.c_int16,C.c_uint,C.c_uint,C.c_uint,C.c_uint)
        def env(cmd,data):
            cmd&=0xffff
            if cmd in (9,31):
                C.cast(data,C.POINTER(C.c_char_p))[0]=SYS; return True
            if cmd==10:
                self.fmt=C.cast(data,C.POINTER(C.c_int))[0]; return True
            if cmd==15:
                v=C.cast(data,C.POINTER(Var))[0]; k=v.key.decode()
                if k in self.opts:
                    s=C.c_char_p(self.opts[k].encode()); self._keep.append(s)
                    C.cast(data,C.POINTER(Var))[0].value=s; return True
                return False
            if cmd==3: C.cast(data,C.POINTER(C.c_bool))[0]=True; return True
            return False
        def vid(data,w,h,pitch):
            if data: self.frame=(C.string_at(data,pitch*h),w,h,pitch)
        def st(port,dev,idx,id):
            if port!=0 or dev!=1: return 0
            return 1 if any(B[b]==id for b in self.pressed) else 0
        self.cbs=[ENV(env),VID(vid),AUD(lambda l,r:None),AUDB(lambda d,n:n),POLL(lambda:None),STATE(st)]
        L.retro_set_environment(self.cbs[0]); L.retro_init()
        L.retro_set_video_refresh(self.cbs[1]); L.retro_set_audio_sample(self.cbs[2])
        L.retro_set_audio_sample_batch(self.cbs[3]); L.retro_set_input_poll(self.cbs[4]); L.retro_set_input_state(self.cbs[5])
        gi=GameInfo(game.encode(),None,0,None)
        assert L.retro_load_game(C.byref(gi)), 'falhou ao carregar'
        L.retro_set_controller_port_device(0,1)
        L.retro_get_memory_data.restype=C.c_void_p; L.retro_get_memory_size.restype=C.c_size_t
        L.retro_serialize_size.restype=C.c_size_t
    def run(self,n=1,press=()):
        self.pressed=set(press)
        for _ in range(n): self.L.retro_run()
        self.pressed=set()
    def tap(self,btn,hold=4,after=20):
        self.run(hold,[btn]); self.run(after)
    def ram(self):
        p=self.L.retro_get_memory_data(2); n=self.L.retro_get_memory_size(2)
        return C.string_at(p,n)
    def shot(self,fn):
        data,w,h,pitch=self.frame
        if pitch>=w*4:
            im=Image.frombuffer('RGB',(w,h),data,'raw','BGRX',pitch,1)
        else:
            im=Image.frombuffer('RGB',(w,h),data,'raw','BGR;16',pitch,1)
        im.save(fn); return im
    def save(self):
        n=self.L.retro_serialize_size(); b=C.create_string_buffer(n); assert self.L.retro_serialize(b,n); return b.raw
    def load(self,s):
        b=C.create_string_buffer(s,len(s)); assert self.L.retro_unserialize(b,len(s))
