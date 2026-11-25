"""Minimal SPIR-V module builder (our own): ids, deduplicated types and constants, sections.

Only what the translator needs. Words are emitted little-endian. Version 1.3 by default.
"""
import struct

# Opcodes used by the translator.
OP = dict(
    Nop=0, Undef=1, Name=5, MemberName=6, ExtInstImport=11, ExtInst=12, MemoryModel=14, EntryPoint=15,
    ExecutionMode=16, Capability=17, TypeVoid=19, TypeBool=20, TypeInt=21, TypeFloat=22, TypeVector=23,
    TypeImage=25, TypeSampler=26, TypeSampledImage=27, TypeArray=28, TypeRuntimeArray=29, TypeStruct=30,
    TypePointer=32, TypeFunction=33, ConstantTrue=41, ConstantFalse=42, Constant=43, ConstantComposite=44,
    ConstantNull=46, SpecConstant=50, Function=54, FunctionEnd=56, Variable=59, Load=61, Store=62,
    AccessChain=65, ArrayLength=68, Decorate=71, MemberDecorate=72, VectorShuffle=79, CompositeConstruct=80,
    CompositeExtract=81, CompositeInsert=82, SampledImage=86, ImageSampleImplicitLod=87,
    ImageSampleExplicitLod=88, ImageSampleDrefImplicitLod=89, ImageSampleDrefExplicitLod=90,
    ImageFetch=95, ImageGather=96, ImageDrefGather=97, ImageQuerySizeLod=103, ImageQueryLevels=106,
    Image=100, ImageQueryLod=105, ImageQuerySize=104, ImageRead=98, ImageWrite=99, SpecConstantComposite=51,
    ConvertFToU=109, ConvertFToS=110, ConvertSToF=111, ConvertUToF=112, UConvert=113, SConvert=114,
    FConvert=115, Bitcast=124, SNegate=126, FNegate=127, IAdd=128, FAdd=129, ISub=130, FSub=131,
    IMul=132, FMul=133, UDiv=134, SDiv=135, FDiv=136, UMod=137, IAddCarry=149,
    Select=169, IEqual=170, INotEqual=171, UGreaterThan=172, SGreaterThan=173, UGreaterThanEqual=174,
    SGreaterThanEqual=175, ULessThan=176, SLessThan=177, ULessThanEqual=178, SLessThanEqual=179,
    FOrdEqual=180, FUnordEqual=181, FOrdNotEqual=182, FUnordNotEqual=183, FOrdLessThan=184,
    FUnordLessThan=185, FOrdGreaterThan=186, FUnordGreaterThan=187, FOrdLessThanEqual=188,
    FUnordLessThanEqual=189, FOrdGreaterThanEqual=190, FUnordGreaterThanEqual=191,
    LogicalEqual=164, LogicalNotEqual=165, LogicalOr=166, LogicalAnd=167, LogicalNot=168,
    IsNan=156, IsInf=157,
    ShiftRightLogical=194, ShiftRightArithmetic=195, ShiftLeftLogical=196, BitwiseOr=197, BitwiseXor=198,
    BitwiseAnd=199, Not=200, BitFieldInsert=201, BitFieldSExtract=202, BitFieldUExtract=203,
    BitReverse=204, BitCount=205, DPdx=207, DPdy=208, DPdxFine=210, DPdyFine=211,
    EmitVertex=218, EndPrimitive=219, ControlBarrier=224, MemoryBarrier=225,
    Phi=245, LoopMerge=246, SelectionMerge=247, Label=248, Branch=249, BranchConditional=250, Switch=251,
    Kill=252, Return=253, Unreachable=255,
    GroupNonUniformElect=333, GroupNonUniformAll=334, GroupNonUniformAny=335, GroupNonUniformBallot=339,
    GroupNonUniformShuffle=345, GroupNonUniformShuffleXor=346, GroupNonUniformShuffleUp=347,
    GroupNonUniformShuffleDown=348, GroupNonUniformQuadBroadcast=365, GroupNonUniformQuadSwap=366,
    DemoteToHelperInvocation=5380, IsHelperInvocation=5381,
)

# Enums
CAP = dict(Matrix=0, Shader=1, Geometry=2, Float16=9, Float64=10, Int64=11, ImageGatherExtended=25,
           ClipDistance=32, CullDistance=33, ImageCubeArray=34, SampleRateShading=35, Sampled1D=43,
           Image1D=44, SampledBuffer=46, ImageQuery=50, DerivativeControl=51, InterpolationFunction=52,
           StorageImageExtendedFormats=49, GeometryPointSize=24, MinLod=42, MultiViewport=57,
           GroupNonUniform=61, GroupNonUniformVote=62, GroupNonUniformBallot=64,
           GroupNonUniformShuffle=65, GroupNonUniformQuad=68, ShaderViewportIndexLayerEXT=5254,
           DemoteToHelperInvocation=5379, StorageImageReadWithoutFormat=55,
           StorageImageWriteWithoutFormat=56)
EXEC = dict(Vertex=0, TessellationControl=1, TessellationEvaluation=2, Geometry=3, Fragment=4, GLCompute=5)
STORAGE = dict(UniformConstant=0, Input=1, Uniform=2, Output=3, Workgroup=4, Private=6, Function=7,
               PushConstant=9, StorageBuffer=12)
DEC = dict(RelaxedPrecision=0, SpecId=1, Block=2, BufferBlock=3, ArrayStride=6, BuiltIn=11, NoPerspective=13,
           Flat=14, Centroid=16, Sample=17, Invariant=18, NonWritable=24, NonReadable=25, Location=30,
           Component=31, Index=32, Binding=33, DescriptorSet=34, Offset=35, NoContraction=42)
BUILTIN = dict(Position=0, PointSize=1, ClipDistance=3, CullDistance=4, VertexId=5, InstanceId=6,
               PrimitiveId=7, InvocationId=8, Layer=9, ViewportIndex=10, FragCoord=15, PointCoord=16,
               FrontFacing=17, SampleId=18, SamplePosition=19, SampleMask=20, FragDepth=22,
               HelperInvocation=23, NumWorkgroups=24, WorkgroupSize=25, WorkgroupId=26,
               LocalInvocationId=27, GlobalInvocationId=28, LocalInvocationIndex=29,
               VertexIndex=42, InstanceIndex=43, SubgroupSize=36, SubgroupLocalInvocationId=41)
GLSL = dict(Round=1, RoundEven=2, Trunc=3, FAbs=4, SAbs=5, FSign=6, Floor=8, Ceil=9, Fract=10, Sin=13,
            Cos=14, Exp2=29, Log2=30, Sqrt=31, InverseSqrt=32, FMin=37, UMin=38, SMin=39, FMax=40, UMax=41,
            SMax=42, FClamp=43, Fma=50, PackHalf2x16=58, UnpackHalf2x16=62, FindUMsb=75, FindSMsb=74,
            NMin=79, NMax=80)


class Module:
    def __init__(self, version=(1, 3)):
        self.version = version
        self.next_id = 1
        self.caps = []
        self.exts = []
        self.imports = []
        self.entry = []
        self.modes = []
        self.names = []
        self.decos = []
        self.globals = []       # types, constants, global variables (in order)
        self.funcs = []
        self._types = {}
        self._consts = {}
        self.glsl = None

    def id(self):
        i = self.next_id
        self.next_id += 1
        return i

    @staticmethod
    def ins(buf, op, *words):
        buf.append(((len(words) + 1) << 16) | OP[op] if isinstance(op, str) else ((len(words) + 1) << 16) | op)
        buf.extend(words)

    @staticmethod
    def string(s):
        b = s.encode() + b'\0'
        b += b'\0' * ((4 - len(b) % 4) % 4)
        return list(struct.unpack('<%dI' % (len(b) // 4), b))

    def cap(self, c):
        if c not in self.caps:
            self.caps.append(c)

    def ext(self, e):
        if e not in self.exts:
            self.exts.append(e)

    def glsl_id(self):
        if self.glsl is None:
            self.glsl = self.id()
            self.imports.append((self.glsl, 'GLSL.std.450'))
        return self.glsl

    def name(self, i, s):
        self.ins(self.names, 'Name', i, *self.string(s))

    def deco(self, i, d, *args):
        self.ins(self.decos, 'Decorate', i, DEC[d], *args)

    def mdeco(self, i, m, d, *args):
        self.ins(self.decos, 'MemberDecorate', i, m, DEC[d], *args)

    # ---- types (deduplicated) ----
    def _type(self, key, op, *words):
        t = self._types.get(key)
        if t is None:
            t = self.id()
            self._types[key] = t
            self.ins(self.globals, op, t, *words)
        return t

    def t_void(self):
        return self._type(('void',), 'TypeVoid')

    def t_bool(self):
        return self._type(('bool',), 'TypeBool')

    def t_int(self, w=32, signed=False):
        return self._type(('int', w, signed), 'TypeInt', w, 1 if signed else 0)

    def t_u32(self):
        return self.t_int(32, False)

    def t_i32(self):
        return self.t_int(32, True)

    def t_float(self, w=32):
        return self._type(('float', w), 'TypeFloat', w)

    def t_f32(self):
        return self.t_float(32)

    def t_vec(self, comp, n):
        return self._type(('vec', comp, n), 'TypeVector', comp, n)

    def t_array(self, el, n):
        ln = self.const_u32(n)
        return self._type(('arr', el, n), 'TypeArray', el, ln)

    def t_rtarray(self, el):
        return self._type(('rtarr', el), 'TypeRuntimeArray', el)

    def t_struct(self, key, *members):
        """Struct types are keyed explicitly (decorations differ per use)."""
        return self._type(('struct', key) + members, 'TypeStruct', *members)

    def t_ptr(self, sc, t):
        return self._type(('ptr', sc, t), 'TypePointer', STORAGE[sc], t)

    def t_func(self, ret, *params):
        return self._type(('fn', ret) + params, 'TypeFunction', ret, *params)

    def t_image(self, sampled_type, dim, depth, arrayed, ms, sampled, fmt=0):
        return self._type(('img', sampled_type, dim, depth, arrayed, ms, sampled, fmt), 'TypeImage',
                          sampled_type, dim, depth, arrayed, ms, sampled, fmt)

    def t_sampled_image(self, img):
        return self._type(('simg', img), 'TypeSampledImage', img)

    # ---- constants (deduplicated) ----
    def const(self, t, *words):
        key = ('c', t) + words
        c = self._consts.get(key)
        if c is None:
            c = self.id()
            self._consts[key] = c
            self.ins(self.globals, 'Constant', t, c, *words)
        return c

    def const_u32(self, v):
        return self.const(self.t_u32(), v & 0xFFFFFFFF)

    def const_i32(self, v):
        return self.const(self.t_i32(), v & 0xFFFFFFFF)

    def const_f32_bits(self, bits):
        return self.const(self.t_f32(), bits & 0xFFFFFFFF)

    def const_f32(self, f):
        return self.const_f32_bits(struct.unpack('<I', struct.pack('<f', f))[0])

    def const_bool(self, b):
        key = ('cb', bool(b))
        c = self._consts.get(key)
        if c is None:
            c = self.id()
            self._consts[key] = c
            self.ins(self.globals, 'ConstantTrue' if b else 'ConstantFalse', self.t_bool(), c)
        return c

    def const_composite(self, t, *parts):
        key = ('cc', t) + parts
        c = self._consts.get(key)
        if c is None:
            c = self.id()
            self._consts[key] = c
            self.ins(self.globals, 'ConstantComposite', t, c, *parts)
        return c

    def const_null(self, t):
        key = ('cn', t)
        c = self._consts.get(key)
        if c is None:
            c = self.id()
            self._consts[key] = c
            self.ins(self.globals, 'ConstantNull', t, c)
        return c

    def spec_u32(self, spec_id, default):
        c = self.id()
        self.ins(self.globals, 'SpecConstant', self.t_u32(), c, default)
        self.deco(c, 'SpecId', spec_id)
        return c

    def spec_composite(self, t, *parts):
        c = self.id()
        self.ins(self.globals, 'SpecConstantComposite', t, c, *parts)
        return c

    def gvar(self, ptr_t, sc, init=None):
        v = self.id()
        if init is None:
            self.ins(self.globals, 'Variable', ptr_t, v, STORAGE[sc])
        else:
            self.ins(self.globals, 'Variable', ptr_t, v, STORAGE[sc], init)
        return v

    def assemble(self):
        out = []
        for c in self.caps:
            self.ins(out, 'Capability', CAP[c])
        for e in self.exts:
            self.ins(out, 10, *self.string(e))     # OpExtension = 10
        for i, n in self.imports:
            self.ins(out, 'ExtInstImport', i, *self.string(n))
        self.ins(out, 'MemoryModel', 0, 1)         # Logical, GLSL450
        out += self.entry
        out += self.modes
        out += self.names
        out += self.decos
        out += self.globals
        for f in self.funcs:
            out += f
        hdr = [0x07230203, (self.version[0] << 16) | (self.version[1] << 8), 0x00530052, self.next_id, 0]
        words = hdr + out
        return struct.pack('<%dI' % len(words), *words)
