// multiview_probe: headless kill test for Vulkan multiview (VK_KHR_multiview, core 1.1) on the MoltenVK
// release the game ships (1.4.2), before DXVK 1.9.2 is taught to render both eyes from one submission.
//
//   tools/multiview_probe/build.sh                     # -> build/multiview_probe
//   build/multiview_probe [--size WxH] [--draws N] [--reps N] [--dump DIR]
//
// No window, no Vulkan loader: libMoltenVK.dylib is linked directly as the driver, as the game's Mac build
// does. Everything renders into offscreen images that are copied back to host memory and checked on the CPU.
// Checks (see README.md for what each one means for the DXVK plan):
//   1  multiview feature + limits
//   2  view mask 0b11 into 2-layer colour + depth: per-view x shift via ubo.shift[gl_ViewIndex], depth per layer
//   3a sampler2DArray at layer gl_ViewIndex from the 2-layer result of 2: layer N in == layer N out
//   3b same from a 1-layer texture seen through a 2D_ARRAY view: is layer 1 clamped to layer 0?
//   4  vkCmdClearAttachments inside the multiview pass clears both layers (and a draw after it still works)
//   5a instanceCount 3 with gl_InstanceIndex offsets: 3 instances per layer (MoltenVK's instance remap)
//   5b the same with a per-instance (VK_VERTEX_INPUT_RATE_INSTANCE) attribute, as D3D9 instancing uses
//   6  timing: N draws mono, N draws multiview, 2 x N draws in two mono passes
//   7  which Metal path MoltenVK used (informational)
// Exit status: 0 all PASS, 1 any FAIL, 2 setup error (Vulkan call failed, no device, bad arguments).
//
// The SPIR-V is assembled by hand below (no glslang in the repo or the MoltenVK package); each module has
// its GLSL equivalent in a comment.

#include <Metal/Metal.h>  // MTLLanguageVersion etc. are used by MVKPhysicalDeviceMetalFeatures below
#include <vulkan/vulkan.h>
#include <MoltenVK/mvk_deprecated_api.h>  // MVKPhysicalDeviceMetalFeatures; also pulls in mvk_private_api.h

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------------
// Minimal SPIR-V assembler.
//
// Numeric values below were checked against spirv.hpp (SPIR-V 1.4 headers in DXVK's include/spirv). They are
// written out here so the probe builds without the DXVK submodule checked out.
// ---------------------------------------------------------------------------------------------------------
namespace sp {
enum : uint32_t {
    MagicNumber = 0x07230203,
    Version10 = 0x00010000,  // SPIR-V 1.0: what DXVK 1.9.2 emits; multiview then needs SPV_KHR_multiview
    // Opcodes
    OpExtension = 10,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeImage = 25,
    OpTypeSampledImage = 27,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpVectorExtractDynamic = 77,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpImageSampleExplicitLod = 88,
    OpConvertSToF = 111,
    OpFAdd = 129,
    OpFMul = 133,
    OpLabel = 248,
    OpReturn = 253,
    // Capability
    CapabilityShader = 1,
    CapabilityMultiView = 4439,
    // ExecutionModel
    ExecutionModelVertex = 0,
    ExecutionModelFragment = 4,
    // Addressing / memory model
    AddressingModelLogical = 0,
    MemoryModelGLSL450 = 1,
    // ExecutionMode
    ExecutionModeOriginUpperLeft = 7,
    // StorageClass
    StorageClassUniformConstant = 0,
    StorageClassInput = 1,
    StorageClassUniform = 2,
    StorageClassOutput = 3,
    StorageClassPushConstant = 9,
    // Decoration
    DecorationBlock = 2,
    DecorationBuiltIn = 11,
    DecorationFlat = 14,
    DecorationLocation = 30,
    DecorationBinding = 33,
    DecorationDescriptorSet = 34,
    DecorationOffset = 35,
    // BuiltIn
    BuiltInPosition = 0,
    BuiltInFragCoord = 15,
    BuiltInInstanceIndex = 43,
    BuiltInViewIndex = 4440,
    // Image
    Dim2D = 1,
    ImageFormatUnknown = 0,
    ImageOperandsLodMask = 0x2,
    FunctionControlMaskNone = 0,
};
}  // namespace sp

// Builds one module. Instructions go into per-section word lists that finish() concatenates in the order the
// SPIR-V spec's "logical layout of a module" requires. Non-aggregate types and constants are de-duplicated
// (SPIR-V forbids two identical non-aggregate type declarations); structs are always new.
class SpvModule {
public:
    uint32_t newId() { return bound_++; }

    void capability(uint32_t cap) { emit(caps_, sp::OpCapability, {cap}); }
    void extension(const char* name) { emit(exts_, sp::OpExtension, str(name)); }
    void memoryModel() { emit(memModel_, sp::OpMemoryModel, {sp::AddressingModelLogical, sp::MemoryModelGLSL450}); }

    // OpTypeXxx %result <operands...>
    uint32_t type(uint32_t opcode, const std::vector<uint32_t>& operands) {
        std::vector<uint32_t> key = {opcode};
        key.insert(key.end(), operands.begin(), operands.end());
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        uint32_t rid = newId();
        std::vector<uint32_t> ops = {rid};
        ops.insert(ops.end(), operands.begin(), operands.end());
        emit(globals_, opcode, ops);
        cache_[key] = rid;
        return rid;
    }
    uint32_t pointer(uint32_t storageClass, uint32_t pointee) { return type(sp::OpTypePointer, {storageClass, pointee}); }
    uint32_t structType(const std::vector<uint32_t>& members) {
        uint32_t rid = newId();
        std::vector<uint32_t> ops = {rid};
        ops.insert(ops.end(), members.begin(), members.end());
        emit(globals_, sp::OpTypeStruct, ops);
        return rid;
    }
    // OpConstant %type %result <bits>
    uint32_t constant(uint32_t typeId, uint32_t bits) {
        std::vector<uint32_t> key = {sp::OpConstant, typeId, bits};
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        uint32_t rid = newId();
        emit(globals_, sp::OpConstant, {typeId, rid, bits});
        cache_[key] = rid;
        return rid;
    }
    uint32_t constF(uint32_t floatType, float value) {
        uint32_t bits;
        memcpy(&bits, &value, 4);
        return constant(floatType, bits);
    }
    uint32_t constI(uint32_t intType, int32_t value) { return constant(intType, (uint32_t)value); }
    // OpVariable %ptrType %result StorageClass   (module-scope only)
    uint32_t variable(uint32_t ptrType, uint32_t storageClass) {
        uint32_t rid = newId();
        emit(globals_, sp::OpVariable, {ptrType, rid, storageClass});
        return rid;
    }
    void decorate(uint32_t target, uint32_t decoration, const std::vector<uint32_t>& extra = {}) {
        std::vector<uint32_t> ops = {target, decoration};
        ops.insert(ops.end(), extra.begin(), extra.end());
        emit(annotations_, sp::OpDecorate, ops);
    }
    void memberDecorate(uint32_t structId, uint32_t member, uint32_t decoration, const std::vector<uint32_t>& extra = {}) {
        std::vector<uint32_t> ops = {structId, member, decoration};
        ops.insert(ops.end(), extra.begin(), extra.end());
        emit(annotations_, sp::OpMemberDecorate, ops);
    }

    // void main() { ... } — begin/end. Returns the function id (used by OpEntryPoint).
    uint32_t beginMain(uint32_t voidType, uint32_t fnType) {
        uint32_t rid = newId();
        emit(code_, sp::OpFunction, {voidType, rid, sp::FunctionControlMaskNone, fnType});
        emit(code_, sp::OpLabel, {newId()});
        return rid;
    }
    void endMain() {
        emit(code_, sp::OpReturn, {});
        emit(code_, sp::OpFunctionEnd, {});
    }
    // Instruction with a result: <opcode> %resultType %result <operands...>
    uint32_t op(uint32_t opcode, uint32_t resultType, const std::vector<uint32_t>& operands) {
        uint32_t rid = newId();
        std::vector<uint32_t> ops = {resultType, rid};
        ops.insert(ops.end(), operands.begin(), operands.end());
        emit(code_, opcode, ops);
        return rid;
    }
    void store(uint32_t pointerId, uint32_t valueId) { emit(code_, sp::OpStore, {pointerId, valueId}); }

    void entryPoint(uint32_t model, uint32_t fn, const char* name, const std::vector<uint32_t>& interface) {
        std::vector<uint32_t> ops = {model, fn};
        std::vector<uint32_t> s = str(name);
        ops.insert(ops.end(), s.begin(), s.end());
        ops.insert(ops.end(), interface.begin(), interface.end());
        emit(entries_, sp::OpEntryPoint, ops);
    }
    void executionMode(uint32_t fn, uint32_t mode) { emit(execModes_, sp::OpExecutionMode, {fn, mode}); }

    std::vector<uint32_t> finish() const {
        std::vector<uint32_t> w = {sp::MagicNumber, sp::Version10, 0 /*generator*/, bound_, 0 /*schema*/};
        for (const std::vector<uint32_t>* s : {&caps_, &exts_, &memModel_, &entries_, &execModes_, &annotations_, &globals_, &code_})
            w.insert(w.end(), s->begin(), s->end());
        return w;
    }

private:
    static void emit(std::vector<uint32_t>& out, uint32_t opcode, const std::vector<uint32_t>& operands) {
        out.push_back((uint32_t)((operands.size() + 1) << 16) | opcode);  // word count includes this word
        out.insert(out.end(), operands.begin(), operands.end());
    }
    // Literal string: UTF-8 bytes, NUL terminated, zero padded to a word, first byte in the low-order bits.
    static std::vector<uint32_t> str(const char* s) {
        size_t n = strlen(s);
        std::vector<uint32_t> words((n + 1 + 3) / 4, 0u);
        for (size_t i = 0; i < n; ++i) words[i / 4] |= (uint32_t)(uint8_t)s[i] << (8 * (i % 4));
        return words;
    }

    uint32_t bound_ = 1;
    std::map<std::vector<uint32_t>, uint32_t> cache_;
    std::vector<uint32_t> caps_, exts_, memModel_, entries_, execModes_, annotations_, globals_, code_;
};

// Vertex shader. GLSL equivalent (multiview = true, instanceAttr = false):
//
//   #version 450
//   #extension GL_EXT_multiview : require
//   layout(location = 0) in vec2 inPos;
//   layout(set = 0, binding = 0) uniform Ubo { vec4 shift; } ubo;                 // x: view 0, y: view 1
//   layout(push_constant) uniform Pc { vec4 xform; vec4 color; } pc;              // xform: dx, dy, z, inst step
//   layout(location = 0) out vec4 vColor;
//   void main() {
//       float s  = ubo.shift[gl_ViewIndex];                                       // mono variant: ubo.shift.x
//       float dx = float(gl_InstanceIndex) * pc.xform.w;                          // instanceAttr: inInst.x
//       gl_Position = vec4(inPos.x + pc.xform.x + s + dx, inPos.y + pc.xform.y /* + inInst.y */, pc.xform.z, 1.0);
//       vColor = pc.color;
//   }
//
// instanceAttr = true adds `layout(location = 1) in vec2 inInst;` (bound at VK_VERTEX_INPUT_RATE_INSTANCE)
// and uses it instead of gl_InstanceIndex.
static std::vector<uint32_t> buildVertexShader(bool multiview, bool instanceAttr) {
    SpvModule m;
    m.capability(sp::CapabilityShader);
    if (multiview) {
        m.capability(sp::CapabilityMultiView);
        m.extension("SPV_KHR_multiview");
    }
    m.memoryModel();

    const uint32_t tVoid = m.type(sp::OpTypeVoid, {});
    const uint32_t tFn = m.type(sp::OpTypeFunction, {tVoid});
    const uint32_t tF = m.type(sp::OpTypeFloat, {32});
    const uint32_t tI = m.type(sp::OpTypeInt, {32, 1});  // signed: glslang declares gl_ViewIndex / gl_InstanceIndex as int
    const uint32_t tV2 = m.type(sp::OpTypeVector, {tF, 2});
    const uint32_t tV4 = m.type(sp::OpTypeVector, {tF, 4});

    const uint32_t pInV2 = m.pointer(sp::StorageClassInput, tV2);
    const uint32_t pInI = m.pointer(sp::StorageClassInput, tI);
    const uint32_t pOutV4 = m.pointer(sp::StorageClassOutput, tV4);

    std::vector<uint32_t> iface;
    const uint32_t vInPos = m.variable(pInV2, sp::StorageClassInput);
    m.decorate(vInPos, sp::DecorationLocation, {0});
    iface.push_back(vInPos);

    uint32_t vInInst = 0, vInstanceIndex = 0, vViewIndex = 0;
    if (instanceAttr) {
        vInInst = m.variable(pInV2, sp::StorageClassInput);
        m.decorate(vInInst, sp::DecorationLocation, {1});
        iface.push_back(vInInst);
    } else {
        vInstanceIndex = m.variable(pInI, sp::StorageClassInput);
        m.decorate(vInstanceIndex, sp::DecorationBuiltIn, {sp::BuiltInInstanceIndex});
        iface.push_back(vInstanceIndex);
    }
    if (multiview) {
        vViewIndex = m.variable(pInI, sp::StorageClassInput);
        m.decorate(vViewIndex, sp::DecorationBuiltIn, {sp::BuiltInViewIndex});
        iface.push_back(vViewIndex);
    }
    const uint32_t vPosition = m.variable(pOutV4, sp::StorageClassOutput);
    m.decorate(vPosition, sp::DecorationBuiltIn, {sp::BuiltInPosition});
    iface.push_back(vPosition);
    const uint32_t vColor = m.variable(pOutV4, sp::StorageClassOutput);
    m.decorate(vColor, sp::DecorationLocation, {0});
    iface.push_back(vColor);

    // uniform Ubo { vec4 shift; }  set 0 binding 0
    const uint32_t sUbo = m.structType({tV4});
    m.decorate(sUbo, sp::DecorationBlock);
    m.memberDecorate(sUbo, 0, sp::DecorationOffset, {0});
    const uint32_t vUbo = m.variable(m.pointer(sp::StorageClassUniform, sUbo), sp::StorageClassUniform);
    m.decorate(vUbo, sp::DecorationDescriptorSet, {0});
    m.decorate(vUbo, sp::DecorationBinding, {0});

    // push_constant Pc { vec4 xform; vec4 color; }
    const uint32_t sPc = m.structType({tV4, tV4});
    m.decorate(sPc, sp::DecorationBlock);
    m.memberDecorate(sPc, 0, sp::DecorationOffset, {0});
    m.memberDecorate(sPc, 1, sp::DecorationOffset, {16});
    const uint32_t vPc = m.variable(m.pointer(sp::StorageClassPushConstant, sPc), sp::StorageClassPushConstant);

    const uint32_t pUniV4 = m.pointer(sp::StorageClassUniform, tV4);
    const uint32_t pPcV4 = m.pointer(sp::StorageClassPushConstant, tV4);
    const uint32_t cI0 = m.constI(tI, 0);
    const uint32_t cI1 = m.constI(tI, 1);
    const uint32_t cF1 = m.constF(tF, 1.0f);

    const uint32_t fnMain = m.beginMain(tVoid, tFn);
    const uint32_t pos = m.op(sp::OpLoad, tV2, {vInPos});
    const uint32_t px = m.op(sp::OpCompositeExtract, tF, {pos, 0});
    const uint32_t py = m.op(sp::OpCompositeExtract, tF, {pos, 1});

    const uint32_t xform = m.op(sp::OpLoad, tV4, {m.op(sp::OpAccessChain, pPcV4, {vPc, cI0})});
    const uint32_t ox = m.op(sp::OpCompositeExtract, tF, {xform, 0});
    const uint32_t oy = m.op(sp::OpCompositeExtract, tF, {xform, 1});
    const uint32_t oz = m.op(sp::OpCompositeExtract, tF, {xform, 2});
    const uint32_t ow = m.op(sp::OpCompositeExtract, tF, {xform, 3});

    const uint32_t shift = m.op(sp::OpLoad, tV4, {m.op(sp::OpAccessChain, pUniV4, {vUbo, cI0})});
    uint32_t s;
    if (multiview) {
        const uint32_t viewIndex = m.op(sp::OpLoad, tI, {vViewIndex});
        s = m.op(sp::OpVectorExtractDynamic, tF, {shift, viewIndex});  // shift[gl_ViewIndex]
    } else {
        s = m.op(sp::OpCompositeExtract, tF, {shift, 0});
    }

    uint32_t x = m.op(sp::OpFAdd, tF, {px, ox});
    x = m.op(sp::OpFAdd, tF, {x, s});
    uint32_t y = m.op(sp::OpFAdd, tF, {py, oy});
    if (instanceAttr) {
        const uint32_t inst = m.op(sp::OpLoad, tV2, {vInInst});
        x = m.op(sp::OpFAdd, tF, {x, m.op(sp::OpCompositeExtract, tF, {inst, 0})});
        y = m.op(sp::OpFAdd, tF, {y, m.op(sp::OpCompositeExtract, tF, {inst, 1})});
    } else {
        const uint32_t instanceIndex = m.op(sp::OpLoad, tI, {vInstanceIndex});
        const uint32_t fi = m.op(sp::OpConvertSToF, tF, {instanceIndex});
        x = m.op(sp::OpFAdd, tF, {x, m.op(sp::OpFMul, tF, {fi, ow})});
    }
    m.store(vPosition, m.op(sp::OpCompositeConstruct, tV4, {x, y, oz, cF1}));
    m.store(vColor, m.op(sp::OpLoad, tV4, {m.op(sp::OpAccessChain, pPcV4, {vPc, cI1})}));
    m.endMain();

    m.entryPoint(sp::ExecutionModelVertex, fnMain, "main", iface);
    return m.finish();
}

// Fragment shader, flat colour:
//   layout(location = 0) in vec4 vColor;  layout(location = 0) out vec4 outColor;
//   void main() { outColor = vColor; }
static std::vector<uint32_t> buildColorFragmentShader() {
    SpvModule m;
    m.capability(sp::CapabilityShader);
    m.memoryModel();
    const uint32_t tVoid = m.type(sp::OpTypeVoid, {});
    const uint32_t tFn = m.type(sp::OpTypeFunction, {tVoid});
    const uint32_t tF = m.type(sp::OpTypeFloat, {32});
    const uint32_t tV4 = m.type(sp::OpTypeVector, {tF, 4});
    const uint32_t vIn = m.variable(m.pointer(sp::StorageClassInput, tV4), sp::StorageClassInput);
    m.decorate(vIn, sp::DecorationLocation, {0});
    const uint32_t vOut = m.variable(m.pointer(sp::StorageClassOutput, tV4), sp::StorageClassOutput);
    m.decorate(vOut, sp::DecorationLocation, {0});
    const uint32_t fnMain = m.beginMain(tVoid, tFn);
    m.store(vOut, m.op(sp::OpLoad, tV4, {vIn}));
    m.endMain();
    m.entryPoint(sp::ExecutionModelFragment, fnMain, "main", {vIn, vOut});
    m.executionMode(fnMain, sp::ExecutionModeOriginUpperLeft);
    return m.finish();
}

// Fragment shader sampling an array texture at the layer of the current view:
//   #extension GL_EXT_multiview : require
//   layout(set = 0, binding = 1) uniform sampler2DArray tex;
//   layout(push_constant) uniform Pc { vec4 xform; vec4 color; vec4 invSize; } pc;
//   layout(location = 0) out vec4 outColor;
//   void main() {
//       vec2 uv = gl_FragCoord.xy * pc.invSize.xy;
//       outColor = textureLod(tex, vec3(uv, float(gl_ViewIndex)), 0.0);
//   }
// textureLod (OpImageSampleExplicitLod), not texelFetch: for sampling instructions Vulkan defines the array
// layer as clamp(round(z), 0, layers - 1), so an out-of-range layer must read the last layer. texelFetch
// with an out-of-range layer is undefined in Vulkan itself, so it would not tell us anything about MoltenVK.
static std::vector<uint32_t> buildSampleArrayFragmentShader() {
    SpvModule m;
    m.capability(sp::CapabilityShader);
    m.capability(sp::CapabilityMultiView);
    m.extension("SPV_KHR_multiview");
    m.memoryModel();
    const uint32_t tVoid = m.type(sp::OpTypeVoid, {});
    const uint32_t tFn = m.type(sp::OpTypeFunction, {tVoid});
    const uint32_t tF = m.type(sp::OpTypeFloat, {32});
    const uint32_t tI = m.type(sp::OpTypeInt, {32, 1});
    const uint32_t tV3 = m.type(sp::OpTypeVector, {tF, 3});
    const uint32_t tV4 = m.type(sp::OpTypeVector, {tF, 4});
    // OpTypeImage: sampled type, Dim, Depth=0, Arrayed=1, MS=0, Sampled=1 (used with a sampler), format Unknown
    const uint32_t tImg = m.type(sp::OpTypeImage, {tF, sp::Dim2D, 0, 1, 0, 1, sp::ImageFormatUnknown});
    const uint32_t tSImg = m.type(sp::OpTypeSampledImage, {tImg});

    const uint32_t vTex = m.variable(m.pointer(sp::StorageClassUniformConstant, tSImg), sp::StorageClassUniformConstant);
    m.decorate(vTex, sp::DecorationDescriptorSet, {0});
    m.decorate(vTex, sp::DecorationBinding, {1});

    const uint32_t vFragCoord = m.variable(m.pointer(sp::StorageClassInput, tV4), sp::StorageClassInput);
    m.decorate(vFragCoord, sp::DecorationBuiltIn, {sp::BuiltInFragCoord});
    const uint32_t vViewIndex = m.variable(m.pointer(sp::StorageClassInput, tI), sp::StorageClassInput);
    m.decorate(vViewIndex, sp::DecorationBuiltIn, {sp::BuiltInViewIndex});
    m.decorate(vViewIndex, sp::DecorationFlat);  // integer fragment input; glslang declares it `flat in int`
    const uint32_t vOut = m.variable(m.pointer(sp::StorageClassOutput, tV4), sp::StorageClassOutput);
    m.decorate(vOut, sp::DecorationLocation, {0});

    const uint32_t sPc = m.structType({tV4, tV4, tV4});
    m.decorate(sPc, sp::DecorationBlock);
    m.memberDecorate(sPc, 0, sp::DecorationOffset, {0});
    m.memberDecorate(sPc, 1, sp::DecorationOffset, {16});
    m.memberDecorate(sPc, 2, sp::DecorationOffset, {32});
    const uint32_t vPc = m.variable(m.pointer(sp::StorageClassPushConstant, sPc), sp::StorageClassPushConstant);
    const uint32_t pPcV4 = m.pointer(sp::StorageClassPushConstant, tV4);
    const uint32_t cI2 = m.constI(tI, 2);
    const uint32_t cF0 = m.constF(tF, 0.0f);

    const uint32_t fnMain = m.beginMain(tVoid, tFn);
    const uint32_t fc = m.op(sp::OpLoad, tV4, {vFragCoord});
    const uint32_t inv = m.op(sp::OpLoad, tV4, {m.op(sp::OpAccessChain, pPcV4, {vPc, cI2})});
    const uint32_t u = m.op(sp::OpFMul, tF, {m.op(sp::OpCompositeExtract, tF, {fc, 0}), m.op(sp::OpCompositeExtract, tF, {inv, 0})});
    const uint32_t v = m.op(sp::OpFMul, tF, {m.op(sp::OpCompositeExtract, tF, {fc, 1}), m.op(sp::OpCompositeExtract, tF, {inv, 1})});
    const uint32_t layer = m.op(sp::OpConvertSToF, tF, {m.op(sp::OpLoad, tI, {vViewIndex})});
    const uint32_t coord = m.op(sp::OpCompositeConstruct, tV3, {u, v, layer});
    const uint32_t sampledImage = m.op(sp::OpLoad, tSImg, {vTex});
    const uint32_t texel = m.op(sp::OpImageSampleExplicitLod, tV4, {sampledImage, coord, sp::ImageOperandsLodMask, cF0});
    m.store(vOut, texel);
    m.endMain();

    m.entryPoint(sp::ExecutionModelFragment, fnMain, "main", {vFragCoord, vViewIndex, vOut});
    m.executionMode(fnMain, sp::ExecutionModeOriginUpperLeft);
    return m.finish();
}

// ---------------------------------------------------------------------------------------------------------
// Vulkan plumbing
// ---------------------------------------------------------------------------------------------------------
#define VK_CHECK(expr)                                                                                  \
    do {                                                                                                \
        VkResult vkr_ = (expr);                                                                         \
        if (vkr_ != VK_SUCCESS) {                                                                       \
            fprintf(stderr, "FATAL: %s returned VkResult %d (line %d)\n", #expr, (int)vkr_, __LINE__); \
            exit(2);                                                                                    \
        }                                                                                               \
    } while (0)

static void fatal(const char* msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(2);
}

struct Ctx {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};
static Ctx g;

static uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (g.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    fatal("no suitable memory type");
    return 0;
}

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceSize size = 0;
};

// Host-visible, host-coherent, persistently mapped.
static Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(g.device, &bi, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.device, b.buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(g.device, &ai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(g.device, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(g.device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
    return b;
}

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, layers = 0;
};

static Image createImage(uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt, VkImageUsageFlags usage) {
    Image im;
    im.w = w;
    im.h = h;
    im.layers = layers;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g.device, &ci, nullptr, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, im.img, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g.device, &ai, nullptr, &im.mem));
    VK_CHECK(vkBindImageMemory(g.device, im.img, im.mem, 0));
    return im;
}

static VkImageView createView(const Image& im, VkImageViewType type, VkFormat fmt, VkImageAspectFlags aspect,
                              uint32_t baseLayer, uint32_t layerCount) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im.img;
    vi.viewType = type;
    vi.format = fmt;
    vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY};
    vi.subresourceRange = {aspect, 0, 1, baseLayer, layerCount};
    VkImageView view;
    VK_CHECK(vkCreateImageView(g.device, &vi, nullptr, &view));
    return view;
}

static void imageBarrier(VkImage img, VkImageAspectFlags aspect, uint32_t layers, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                         VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    vkCmdPipelineBarrier(g.cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void beginCmd() {
    VK_CHECK(vkResetCommandBuffer(g.cmd, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(g.cmd, &bi));
}

static void submitAndWait() {
    VK_CHECK(vkEndCommandBuffer(g.cmd));
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    VK_CHECK(vkQueueSubmit(g.queue, 1, &si, g.fence));
    VK_CHECK(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(g.device, 1, &g.fence));
}

// Records a copy of every layer of an RGBA8 colour image (currently in TRANSFER_SRC_OPTIMAL) into `rb`,
// tightly packed, layer after layer, plus the barrier that makes it visible to the host after the fence.
static void recordReadback(const Image& im, const Buffer& rb) {
    VkBufferImageCopy r{};
    r.bufferOffset = 0;
    r.bufferRowLength = 0;    // tightly packed rows
    r.bufferImageHeight = 0;  // layer stride = w * h * 4
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im.layers};
    r.imageOffset = {0, 0, 0};
    r.imageExtent = {im.w, im.h, 1};
    vkCmdCopyImageToBuffer(g.cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = rb.buf;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
}

// One subpass; colour RGBA8 (cleared, stored, ends in TRANSFER_SRC_OPTIMAL for the readback) and optionally
// D32 depth (cleared, not stored). viewMask != 0 makes it a multiview render pass (VkRenderPassMultiviewCreateInfo,
// the Vulkan 1.1 core form of VK_KHR_multiview; DXVK 1.9.2 creates render passes with vkCreateRenderPass too).
static VkRenderPass createRenderPass(bool depth, uint32_t viewMask) {
    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = depth ? &depthRef : nullptr;

    // Conservative external dependencies: previous transfers / sampling / attachment writes before this pass,
    // and this pass's attachment writes before later transfers (readback) and sampling (step 3).
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassMultiviewCreateInfo mv{};
    mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
    mv.subpassCount = 1;
    mv.pViewMasks = &viewMask;
    mv.dependencyCount = 0;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &viewMask;  // the two eyes are spatially correlated (a hint only)

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.pNext = viewMask ? &mv : nullptr;
    rp.attachmentCount = depth ? 2 : 1;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VkRenderPass pass;
    VK_CHECK(vkCreateRenderPass(g.device, &rp, nullptr, &pass));
    return pass;
}

// For a multiview render pass the attachments are 2D_ARRAY views covering all views and layers must be 1.
static VkFramebuffer createFramebuffer(VkRenderPass rp, const std::vector<VkImageView>& views, uint32_t w, uint32_t h) {
    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = rp;
    fi.attachmentCount = (uint32_t)views.size();
    fi.pAttachments = views.data();
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VkFramebuffer fb;
    VK_CHECK(vkCreateFramebuffer(g.device, &fi, nullptr, &fb));
    return fb;
}

static VkShaderModule createShader(const std::vector<uint32_t>& words) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words.size() * sizeof(uint32_t);
    ci.pCode = words.data();
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(g.device, &ci, nullptr, &mod));
    return mod;
}

static VkPipeline createPipeline(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs,
                                 bool depth, bool instanceAttr) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binds[2] = {
        {0, 8, VK_VERTEX_INPUT_RATE_VERTEX},
        {1, 8, VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {1, 1, VK_FORMAT_R32G32_SFLOAT, 0},
    };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = instanceAttr ? 2 : 1;
    vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = instanceAttr ? 2 : 1;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = depth ? &ds : nullptr;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dyn;
    pi.layout = layout;
    pi.renderPass = rp;
    pi.subpass = 0;
    VkPipeline pipe;
    VK_CHECK(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe));
    return pipe;
}

// ---------------------------------------------------------------------------------------------------------
// Scene description shared by the recording code and the CPU checks
// ---------------------------------------------------------------------------------------------------------
struct PushConstants {  // matches both shaders' push_constant blocks
    float xform[4];     // dx, dy, z, per-instance x step (NDC)
    float color[4];
    float invSize[4];   // 1/W, 1/H (sampling shader only)
};
static_assert(sizeof(PushConstants) == 48, "push constant layout");

// Vertex buffer: five shapes of three vec2 vertices each (NDC; Vulkan NDC y points down).
enum { kRedFirst = 0, kGreenFirst = 3, kInstFirst = 6, kTinyFirst = 9, kFullscreenFirst = 12, kVertexCount = 15 };
static const float kVertices[kVertexCount * 2] = {
    -0.40f, 0.30f,  0.40f, 0.30f,  0.00f, -0.50f,  // red, far:   centroid (0, 0.0333), area 0.08 W H px
    -0.10f, 0.10f,  0.10f, 0.10f,  0.00f, -0.10f,  // green, near: inside red, centroid (0, 0.0333), area 0.005 W H
    -0.08f, 0.08f,  0.08f, 0.08f,  0.00f, -0.08f,  // instanced: area 0.0032 W H, 0.04 NDC half width at y = 0
    -0.01f, 0.01f,  0.01f, 0.01f,  0.00f, -0.01f,  // timing
    -1.00f, -1.00f, 3.00f, -1.00f, -1.00f, 3.00f,  // full-screen triangle
};
static const float kTriCentroidY = (0.30f + 0.30f - 0.50f) / 3.0f;  // red and green share it
static const float kShiftNdc = 0.25f;                                // view 0: -0.25, view 1: +0.25
static const float kClearGray = 0.1f;

static const float kRed[4] = {1, 0, 0, 1};
static const float kGreen[4] = {0, 1, 0, 1};

// Pixel classification for RGBA8 readbacks (exact colours expected; thresholds only absorb rounding).
static bool isRed(const uint8_t* p) { return p[0] > 200 && p[1] < 60 && p[2] < 60; }
static bool isGreen(const uint8_t* p) { return p[1] > 200 && p[0] < 60 && p[2] < 60; }
static bool isMagenta(const uint8_t* p) { return p[0] > 200 && p[1] < 60 && p[2] > 200; }
static bool isBackground(const uint8_t* p) { return p[0] < 60 && p[1] < 60 && p[2] < 60; }

static float ndcToPx(float ndc, uint32_t size) { return (ndc + 1.0f) * 0.5f * (float)size; }

// ---------------------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------------------
static int g_failures = 0;
static void report(const char* item, bool pass, const std::string& detail) {
    printf("[%s] %-4s %s\n", item, pass ? "PASS" : "FAIL", detail.c_str());
    if (!pass) ++g_failures;
    fflush(stdout);
}
static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

static std::string g_dumpDir;
static void dumpPPM(const char* name, const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (g_dumpDir.empty()) return;
    std::string path = g_dumpDir + "/" + name + ".ppm";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "warning: cannot write %s: %s\n", path.c_str(), strerror(errno));
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row(w * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) memcpy(&row[x * 3], rgba + ((size_t)y * w + x) * 4, 3);
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n == 0 ? 0.0 : (n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]));
}

// Recursively collects regular files under dir (MoltenVK's shader dump layout is not documented in detail).
static void listFiles(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
    if (depth > 4) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) listFiles(p, out, depth + 1);
        else if (S_ISREG(st.st_mode)) out.push_back(p);
    }
    closedir(d);
}
static std::string readFile(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (64u << 20)) s.append(buf, n);
    fclose(f);
    return s;
}

static void usage() {
    printf("usage: multiview_probe [--size WxH] [--draws N] [--reps N] [--dump DIR]\n"
           "  --size WxH   render target size per view (default 1024x1024)\n"
           "  --draws N    draws per pass for the timing test (default 2000)\n"
           "  --reps N     timed repetitions per timing case, median reported (default 7)\n"
           "  --dump DIR   write every read-back layer as a PPM into DIR\n");
}

// ---------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    uint32_t W = 1024, H = 1024, draws = 2000, reps = 7;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &W, &H) != 2 || W < 64 || H < 64 || W > 16384 || H > 16384) {
                fprintf(stderr, "bad --size (want WxH, 64..16384)\n");
                return 2;
            }
        } else if (a == "--draws" && i + 1 < argc) {
            draws = (uint32_t)strtoul(argv[++i], nullptr, 10);
            if (draws == 0) { fprintf(stderr, "bad --draws\n"); return 2; }
        } else if (a == "--reps" && i + 1 < argc) {
            reps = (uint32_t)strtoul(argv[++i], nullptr, 10);
            if (reps == 0) { fprintf(stderr, "bad --reps\n"); return 2; }
        } else if (a == "--dump" && i + 1 < argc) {
            g_dumpDir = argv[++i];
            if (mkdir(g_dumpDir.c_str(), 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "cannot create %s: %s\n", g_dumpDir.c_str(), strerror(errno));
                return 2;
            }
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    // MoltenVK reads its configuration from the environment once, lazily, at its first use (the game relies
    // on the same thing: simulator/game_app.cpp sets MVK_CONFIG_* before SDL creates the Vulkan instance).
    // Match the game's discrete Metal bindings, and ask MoltenVK to dump the MSL it generates so check 7 can
    // tell which multiview path it took. Values already in the environment win (overwrite = 0).
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
    std::string shaderDumpDir;
    if (const char* d = getenv("MVK_CONFIG_SHADER_DUMP_DIR")) {
        shaderDumpDir = d;
    } else {
        const char* tmp = getenv("TMPDIR");
        std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/multiview_probe_msl.XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data())) {
            shaderDumpDir = buf.data();
            setenv("MVK_CONFIG_SHADER_DUMP_DIR", shaderDumpDir.c_str(), 1);
        }
    }

    printf("multiview_probe: %ux%u per view, %u timing draws, %u reps; MoltenVK headers %s\n", W, H, draws, reps,
           MVK_VERSION_STRING);

    // ---- instance / device -------------------------------------------------------------------------------
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "multiview_probe";
    app.apiVersion = VK_API_VERSION_1_1;  // what DXVK 1.9.2 asks for; multiview is core here
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &g.instance));

    uint32_t physCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(g.instance, &physCount, nullptr));
    if (physCount == 0) fatal("no Vulkan physical device");
    std::vector<VkPhysicalDevice> phys(physCount);
    VK_CHECK(vkEnumeratePhysicalDevices(g.instance, &physCount, phys.data()));
    g.phys = phys[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g.phys, &props);
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.memProps);
    printf("device: %s, Vulkan %u.%u.%u, driverVersion %u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
    if (VK_API_VERSION_MINOR(props.apiVersion) < 1 && VK_API_VERSION_MAJOR(props.apiVersion) == 1)
        fatal("device is Vulkan 1.0; multiview needs 1.1");

    // ---- 1: multiview feature and limits ------------------------------------------------------------------
    VkPhysicalDeviceMultiviewFeatures mvFeat{};
    mvFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &mvFeat;
    vkGetPhysicalDeviceFeatures2(g.phys, &feat2);
    VkPhysicalDeviceMultiviewProperties mvProps{};
    mvProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &mvProps;
    vkGetPhysicalDeviceProperties2(g.phys, &props2);
    const bool mvOk = mvFeat.multiview == VK_TRUE && mvProps.maxMultiviewViewCount >= 2;
    report("1", mvOk,
           fmt("multiview=%u (geometryShader=%u tessellationShader=%u) maxMultiviewViewCount=%u maxMultiviewInstanceIndex=%u",
               mvFeat.multiview, mvFeat.multiviewGeometryShader, mvFeat.multiviewTessellationShader,
               mvProps.maxMultiviewViewCount, mvProps.maxMultiviewInstanceIndex));
    if (!mvOk) {
        printf("multiview unavailable; nothing else to test\n");
        return 1;
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qfCount, qfs.data());
    g.queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.queueFamily = i; break; }
    if (g.queueFamily == UINT32_MAX) fatal("no graphics queue");

    // The portability spec says VK_KHR_portability_subset must be enabled when the device offers it.
    uint32_t extCount = 0;
    VK_CHECK(vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &extCount, nullptr));
    std::vector<VkExtensionProperties> exts(extCount);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &extCount, exts.data()));
    std::vector<const char*> devExts;
    for (const VkExtensionProperties& e : exts)
        if (!strcmp(e.extensionName, "VK_KHR_portability_subset")) devExts.push_back("VK_KHR_portability_subset");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceMultiviewFeatures mvEnable{};
    mvEnable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    mvEnable.multiview = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &mvEnable;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();
    VK_CHECK(vkCreateDevice(g.phys, &dci, nullptr, &g.device));
    vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g.queueFamily;
    VK_CHECK(vkCreateCommandPool(g.device, &cpi, nullptr, &g.pool));
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(g.device, &cbai, &g.cmd));
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(g.device, &fci, nullptr, &g.fence));

    // ---- resources ----------------------------------------------------------------------------------------
    const VkFormat colorFmt = VK_FORMAT_R8G8B8A8_UNORM, depthFmt = VK_FORMAT_D32_SFLOAT;
    const size_t layerBytes = (size_t)W * H * 4;

    // 2-layer scene targets (checks 2, 4, 5 and the multiview / two-pass timing cases).
    Image sceneColor = createImage(W, H, 2, colorFmt,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    Image sceneDepth = createImage(W, H, 2, depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    VkImageView sceneColorArray = createView(sceneColor, VK_IMAGE_VIEW_TYPE_2D_ARRAY, colorFmt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 2);
    VkImageView sceneDepthArray = createView(sceneDepth, VK_IMAGE_VIEW_TYPE_2D_ARRAY, depthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 2);
    VkImageView sceneColorLayer[2], sceneDepthLayer[2];  // per-layer 2D views for the "two mono passes" case
    for (uint32_t l = 0; l < 2; ++l) {
        sceneColorLayer[l] = createView(sceneColor, VK_IMAGE_VIEW_TYPE_2D, colorFmt, VK_IMAGE_ASPECT_COLOR_BIT, l, 1);
        sceneDepthLayer[l] = createView(sceneDepth, VK_IMAGE_VIEW_TYPE_2D, depthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, l, 1);
    }
    // 2-layer output of the sampling pass (check 3).
    Image sampleOut = createImage(W, H, 2, colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView sampleOutArray = createView(sampleOut, VK_IMAGE_VIEW_TYPE_2D_ARRAY, colorFmt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 2);
    // 1-layer mono targets (timing case i).
    Image monoColor = createImage(W, H, 1, colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Image monoDepth = createImage(W, H, 1, depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    VkImageView monoColorView = createView(monoColor, VK_IMAGE_VIEW_TYPE_2D, colorFmt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    VkImageView monoDepthView = createView(monoDepth, VK_IMAGE_VIEW_TYPE_2D, depthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1);
    // 1-layer texture with a known pattern, viewed as a 1-layer 2D_ARRAY (check 3b).
    Image tex1 = createImage(W, H, 1, colorFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageView tex1Array = createView(tex1, VK_IMAGE_VIEW_TYPE_2D_ARRAY, colorFmt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    std::vector<uint8_t> pattern(layerBytes);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &pattern[((size_t)y * W + x) * 4];
            p[0] = (uint8_t)(x & 255);
            p[1] = (uint8_t)(y & 255);
            p[2] = (uint8_t)(((x >> 3) ^ (y >> 3)) & 1 ? 200 : 40);
            p[3] = 255;
        }

    Buffer readback = createBuffer(layerBytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer staging = createBuffer(layerBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(staging.map, pattern.data(), layerBytes);
    Buffer vbuf = createBuffer(sizeof kVertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vbuf.map, kVertices, sizeof kVertices);
    const float instOffsets[6] = {-0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f};  // check 5b, per instance
    Buffer instBuf = createBuffer(sizeof instOffsets, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(instBuf.map, instOffsets, sizeof instOffsets);
    auto makeUbo = [](float s0, float s1) {
        Buffer b = createBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        const float v[4] = {s0, s1, 0.0f, 0.0f};
        memcpy(b.map, v, sizeof v);
        return b;
    };
    Buffer uboScene = makeUbo(-kShiftNdc, kShiftNdc);
    Buffer uboZero = makeUbo(0.0f, 0.0f);

    // Upload the pattern into tex1.
    beginCmd();
    imageBarrier(tex1.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    {
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {W, H, 1};
        vkCmdCopyBufferToImage(g.cmd, staging.buf, tex1.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    imageBarrier(tex1.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    submitAndWait();

    // Sampler: nearest, clamp to edge, single mip. Sampling at texel centres reproduces the texture exactly.
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    VkSampler sampler;
    VK_CHECK(vkCreateSampler(g.device, &sci, nullptr, &sampler));

    // Descriptor layout shared by every pipeline: binding 0 = UBO (vertex), binding 1 = sampler2DArray (fragment).
    VkDescriptorSetLayoutBinding dslb[2]{};
    dslb[0].binding = 0;
    dslb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dslb[0].descriptorCount = 1;
    dslb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    dslb[1].binding = 1;
    dslb[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb[1].descriptorCount = 1;
    dslb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = dslb;
    VkDescriptorSetLayout dsl;
    VK_CHECK(vkCreateDescriptorSetLayout(g.device, &dslci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout playout;
    VK_CHECK(vkCreatePipelineLayout(g.device, &plci, nullptr, &playout));

    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 4;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    VkDescriptorPool dpool;
    VK_CHECK(vkCreateDescriptorPool(g.device, &dpci, nullptr, &dpool));
    // Every set has both bindings written, even where the pipeline only uses the UBO.
    auto makeSet = [&](const Buffer& ubo, VkImageView view) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &dsl;
        VkDescriptorSet set;
        VK_CHECK(vkAllocateDescriptorSets(g.device, &ai, &set));
        VkDescriptorBufferInfo bi{ubo.buf, 0, 16};
        VkDescriptorImageInfo ii{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = set;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet = set;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(g.device, 2, w, 0, nullptr);
        return set;
    };
    VkDescriptorSet setScene = makeSet(uboScene, tex1Array);        // shift -0.25 / +0.25
    VkDescriptorSet setZero = makeSet(uboZero, tex1Array);          // no shift (timing)
    VkDescriptorSet setSampleA = makeSet(uboZero, sceneColorArray); // 3a: the 2-layer result of check 2
    VkDescriptorSet setSampleB = makeSet(uboZero, tex1Array);       // 3b: the 1-layer texture

    VkRenderPass rpScene = createRenderPass(true, 0x3);   // multiview, colour + depth
    VkRenderPass rpSample = createRenderPass(false, 0x3); // multiview, colour only
    VkRenderPass rpMono = createRenderPass(true, 0);      // no multiview
    VkFramebuffer fbScene = createFramebuffer(rpScene, {sceneColorArray, sceneDepthArray}, W, H);
    VkFramebuffer fbSample = createFramebuffer(rpSample, {sampleOutArray}, W, H);
    VkFramebuffer fbMono = createFramebuffer(rpMono, {monoColorView, monoDepthView}, W, H);
    VkFramebuffer fbLayer[2];
    for (uint32_t l = 0; l < 2; ++l) fbLayer[l] = createFramebuffer(rpMono, {sceneColorLayer[l], sceneDepthLayer[l]}, W, H);

    VkShaderModule vsMV = createShader(buildVertexShader(true, false));
    VkShaderModule vsMVInst = createShader(buildVertexShader(true, true));
    VkShaderModule vsMono = createShader(buildVertexShader(false, false));
    VkShaderModule fsColor = createShader(buildColorFragmentShader());
    VkShaderModule fsSample = createShader(buildSampleArrayFragmentShader());
    VkPipeline pColorMV = createPipeline(rpScene, playout, vsMV, fsColor, true, false);
    VkPipeline pColorMVInst = createPipeline(rpScene, playout, vsMVInst, fsColor, true, true);
    VkPipeline pSampleMV = createPipeline(rpSample, playout, vsMV, fsSample, false, false);
    VkPipeline pColorMono = createPipeline(rpMono, playout, vsMono, fsColor, true, false);

    const VkViewport viewport{0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {W, H}};
    VkClearValue clears[2];
    clears[0].color = {{kClearGray, kClearGray, kClearGray, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    auto beginPass = [&](VkRenderPass rp, VkFramebuffer fb, bool depth, VkPipeline pipe, VkDescriptorSet set) {
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = rp;
        rbi.framebuffer = fb;
        rbi.renderArea = scissor;
        rbi.clearValueCount = depth ? 2 : 1;
        rbi.pClearValues = clears;
        vkCmdBeginRenderPass(g.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdSetViewport(g.cmd, 0, 1, &viewport);
        vkCmdSetScissor(g.cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &set, 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(g.cmd, 0, 1, &vbuf.buf, &off);
    };
    auto push = [&](float dx, float dy, float z, float step, const float* color) {
        PushConstants pc{};
        pc.xform[0] = dx; pc.xform[1] = dy; pc.xform[2] = z; pc.xform[3] = step;
        memcpy(pc.color, color, sizeof pc.color);
        pc.invSize[0] = 1.0f / (float)W;
        pc.invSize[1] = 1.0f / (float)H;
        vkCmdPushConstants(g.cmd, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    };
    auto layerPtr = [&](const std::vector<uint8_t>& img, uint32_t l) { return img.data() + l * layerBytes; };
    auto fetch = [&](const char* dumpName) {  // copy the readback buffer out (2 layers) and optionally dump it
        std::vector<uint8_t> v((const uint8_t*)readback.map, (const uint8_t*)readback.map + layerBytes * 2);
        for (uint32_t l = 0; l < 2; ++l) dumpPPM(fmt("%s_layer%u", dumpName, l).c_str(), v.data() + l * layerBytes, W, H);
        return v;
    };
    auto px = [&](const uint8_t* layer, int x, int y) -> const uint8_t* {
        x = std::max(0, std::min((int)W - 1, x));
        y = std::max(0, std::min((int)H - 1, y));
        return layer + ((size_t)y * W + x) * 4;
    };
    const float shiftOf[2] = {-kShiftNdc, kShiftNdc};

    // ---- 2: per-view shift and per-layer depth --------------------------------------------------------------
    // Near green triangle first, then the far red one over it: with a working depth buffer per layer the green
    // stays visible in both layers; with a missing or shared depth layer red covers it in at least one layer.
    beginCmd();
    beginPass(rpScene, fbScene, true, pColorMV, setScene);
    push(0, 0, 0.3f, 0, kGreen);
    vkCmdDraw(g.cmd, 3, 1, kGreenFirst, 0);
    push(0, 0, 0.6f, 0, kRed);
    vkCmdDraw(g.cmd, 3, 1, kRedFirst, 0);
    vkCmdEndRenderPass(g.cmd);
    recordReadback(sceneColor, readback);
    // Leave the scene image ready for sampling in check 3a.
    imageBarrier(sceneColor.img, VK_IMAGE_ASPECT_COLOR_BIT, 2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT);
    submitAndWait();
    const std::vector<uint8_t> scene = fetch("check2_scene");
    {
        bool ok = true;
        std::string detail;
        double cxMeasured[2] = {0, 0};
        for (uint32_t l = 0; l < 2; ++l) {
            const uint8_t* L = layerPtr(scene, l);
            size_t covered = 0, green = 0;
            double sx = 0, sy = 0;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    const uint8_t* p = L + ((size_t)y * W + x) * 4;
                    const bool r = isRed(p), gr = isGreen(p);
                    if (r || gr) { ++covered; sx += x + 0.5; sy += y + 0.5; }
                    if (gr) ++green;
                }
            const double cx = covered ? sx / covered : -1, cy = covered ? sy / covered : -1;
            cxMeasured[l] = cx;
            const double ex = ndcToPx(shiftOf[l], W), ey = ndcToPx(kTriCentroidY, H);
            const double areaRed = 0.08 * W * H, areaGreen = 0.005 * W * H;
            const bool centroidOk = std::fabs(cx - ex) <= 1.5 && std::fabs(cy - ey) <= 1.5;
            const bool areaOk = std::fabs((double)covered - areaRed) <= 0.03 * areaRed;
            const bool greenOk = std::fabs((double)green - areaGreen) <= 0.15 * areaGreen;
            const bool greenCentre = isGreen(px(L, (int)ndcToPx(shiftOf[l], W), (int)ndcToPx(kTriCentroidY, H)));
            ok = ok && centroidOk && areaOk && greenOk && greenCentre;
            detail += fmt("\n       layer %u: centroid (%.2f, %.2f) expected (%.2f, %.2f)%s; covered %zu px (expect ~%.0f)%s; "
                          "green %zu px (expect ~%.0f)%s; green at its centre: %s",
                          l, cx, cy, ex, ey, centroidOk ? "" : " WRONG", covered, areaRed, areaOk ? "" : " WRONG", green,
                          areaGreen, greenOk ? "" : " WRONG (depth test)", greenCentre ? "yes" : "NO");
        }
        const double sep = cxMeasured[1] - cxMeasured[0], expectSep = 2.0 * kShiftNdc * 0.5 * W;
        const bool sepOk = std::fabs(sep - expectSep) <= 1.5 && cxMeasured[0] < W * 0.5 && cxMeasured[1] > W * 0.5;
        ok = ok && sepOk;
        report("2", ok,
               fmt("per-view shift: layer 0 left, layer 1 right, separation %.2f px (expect %.2f)%s; depth occlusion per layer",
                   sep, expectSep, sepOk ? "" : " WRONG") + detail);
    }

    // ---- 3: sampler2DArray at layer gl_ViewIndex ------------------------------------------------------------
    auto runSample = [&](VkDescriptorSet set, const char* dumpName) {
        beginCmd();
        beginPass(rpSample, fbSample, false, pSampleMV, set);
        push(0, 0, 0.0f, 0, kRed);  // xform zero: the full-screen triangle as is; invSize drives the UVs
        vkCmdDraw(g.cmd, 3, 1, kFullscreenFirst, 0);
        vkCmdEndRenderPass(g.cmd);
        recordReadback(sampleOut, readback);
        submitAndWait();
        return fetch(dumpName);
    };
    auto countMismatch = [&](const uint8_t* a, const uint8_t* b) {
        size_t bad = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i)
            for (int c = 0; c < 4; ++c)
                if (std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]) > 1) { ++bad; break; }
        return bad;
    };
    bool check3aOk;
    {
        const std::vector<uint8_t> out = runSample(setSampleA, "check3a_sampled");
        const size_t bad0 = countMismatch(layerPtr(out, 0), layerPtr(scene, 0));
        const size_t bad1 = countMismatch(layerPtr(out, 1), layerPtr(scene, 1));
        const size_t inputsDiffer = countMismatch(layerPtr(scene, 0), layerPtr(scene, 1));
        const size_t crossed = countMismatch(layerPtr(out, 1), layerPtr(scene, 0));
        check3aOk = bad0 == 0 && bad1 == 0 && inputsDiffer > 0;
        report("3a", check3aOk,
               fmt("2-layer source: out layer 0 vs in layer 0: %zu px differ; out layer 1 vs in layer 1: %zu px differ "
                   "(input layers differ in %zu px; out layer 1 vs in layer 0: %zu px differ)",
                   bad0, bad1, inputsDiffer, crossed) +
                   (inputsDiffer == 0 ? " -- inputs identical, check 2 must be fixed first" : ""));
    }
    {
        const std::vector<uint8_t> out = runSample(setSampleB, "check3b_sampled");
        const size_t bad0 = countMismatch(layerPtr(out, 0), pattern.data());
        const size_t bad1 = countMismatch(layerPtr(out, 1), pattern.data());
        size_t zero1 = 0;
        const uint8_t* L1 = layerPtr(out, 1);
        for (size_t i = 0; i < (size_t)W * H; ++i)
            if (L1[i * 4] == 0 && L1[i * 4 + 1] == 0 && L1[i * 4 + 2] == 0) ++zero1;
        const char* verdict;
        if (!check3aOk) verdict = "INCONCLUSIVE (3a failed, so gl_ViewIndex in the fragment shader is not trustworthy)";
        else if (bad1 == 0) verdict = "CLAMPED";
        else if (zero1 == (size_t)W * H) verdict = "NOT CLAMPED (layer 1 reads black/zero)";
        else verdict = "NOT CLAMPED (layer 1 reads something other than layer 0)";
        const uint8_t* c = px(L1, (int)W / 2, (int)H / 2);
        printf("[3b] out-of-range layer: %s\n", verdict);
        report("3b", bad0 == 0 && bad1 == 0 && check3aOk,
               fmt("1-layer source through a 2D_ARRAY view: out layer 0 vs layer 0: %zu px differ; out layer 1 vs layer 0: "
                   "%zu px differ (%zu black); centre of out layer 1 = %u,%u,%u,%u -> %s",
                   bad0, bad1, zero1, (unsigned)c[0], (unsigned)c[1], (unsigned)c[2], (unsigned)c[3], verdict));
    }

    // ---- 4: vkCmdClearAttachments inside the multiview pass -------------------------------------------------
    // Clear a rectangle to magenta (layerCount 1 and baseArrayLayer 0, as multiview requires; it applies to all
    // views in the mask), then draw a green triangle to the right of it, which also checks that MoltenVK's
    // internal clear draw leaves our pipeline state usable.
    {
        const int rx = (int)(W / 8), ry = (int)(H / 8), rw = (int)(3 * W / 8), rh = (int)(3 * H / 4);
        beginCmd();
        beginPass(rpScene, fbScene, true, pColorMV, setScene);
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = 0;
        ca.clearValue.color = {{1.0f, 0.0f, 1.0f, 1.0f}};
        VkClearRect cr{};
        cr.rect = {{rx, ry}, {(uint32_t)rw, (uint32_t)rh}};
        cr.baseArrayLayer = 0;
        cr.layerCount = 1;
        vkCmdClearAttachments(g.cmd, 1, &ca, 1, &cr);
        push(0.5f, 0, 0.3f, 0, kGreen);
        vkCmdDraw(g.cmd, 3, 1, kGreenFirst, 0);
        vkCmdEndRenderPass(g.cmd);
        recordReadback(sceneColor, readback);
        submitAndWait();
        const std::vector<uint8_t> out = fetch("check4_clear");
        bool ok = true;
        std::string detail;
        for (uint32_t l = 0; l < 2; ++l) {
            const uint8_t* L = layerPtr(out, l);
            size_t inNotMagenta = 0, outMagenta = 0, outOther = 0, green = 0;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    const uint8_t* p = L + ((size_t)y * W + x) * 4;
                    const bool inside = (int)x >= rx && (int)x < rx + rw && (int)y >= ry && (int)y < ry + rh;
                    if (isGreen(p)) ++green;
                    if (inside) { if (!isMagenta(p)) ++inNotMagenta; }
                    else if (isMagenta(p)) ++outMagenta;
                    else if (!isBackground(p) && !isGreen(p)) ++outOther;
                }
            const double areaGreen = 0.005 * W * H;
            const bool greenOk = std::fabs((double)green - areaGreen) <= 0.15 * areaGreen &&
                                 isGreen(px(L, (int)ndcToPx(0.5f + shiftOf[l], W), (int)ndcToPx(kTriCentroidY, H)));
            const bool lok = inNotMagenta == 0 && outMagenta == 0 && outOther == 0 && greenOk;
            ok = ok && lok;
            detail += fmt("\n       layer %u: inside rect not magenta %zu px, outside magenta %zu px, other %zu px; "
                          "draw after clear: %zu green px%s",
                          l, inNotMagenta, outMagenta, outOther, green, greenOk ? "" : " WRONG");
        }
        report("4", ok, std::string("vkCmdClearAttachments clears the rect in both layers") + detail);
    }

    // ---- 5: instanced draws in the multiview pass -----------------------------------------------------------
    // Three instances at x = -0.5, 0, +0.5 (NDC) plus the view shift. MoltenVK draws instanceCount x viewCount
    // Metal instances and must hand the shader instance = metal_instance / 2, view = metal_instance % 2.
    auto checkInstances = [&](const std::vector<uint8_t>& out, std::string& detail) {
        bool ok = true;
        const int row = (int)(H / 2);
        const double areaOne = 0.0032 * W * H;
        for (uint32_t l = 0; l < 2; ++l) {
            const uint8_t* L = layerPtr(out, l);
            int runs = 0;
            bool prev = false;
            for (uint32_t x = 0; x < W; ++x) {
                const bool gr = isGreen(px(L, (int)x, row));
                if (gr && !prev) ++runs;
                prev = gr;
            }
            size_t green = 0;
            for (size_t i = 0; i < (size_t)W * H; ++i) green += isGreen(L + i * 4) ? 1 : 0;
            int centresGreen = 0;
            for (int i = 0; i < 3; ++i)
                centresGreen += isGreen(px(L, (int)ndcToPx(-0.5f + 0.5f * i + shiftOf[l], W), row)) ? 1 : 0;
            const bool areaOk = std::fabs((double)green - 3 * areaOne) <= 0.15 * 3 * areaOne;
            const bool lok = runs == 3 && centresGreen == 3 && areaOk;
            ok = ok && lok;
            detail += fmt("\n       layer %u: %d instances on the centre row (expect 3), %d/3 expected centres green, "
                          "%zu green px (expect ~%.0f)%s",
                          l, runs, centresGreen, green, 3 * areaOne, lok ? "" : " WRONG");
        }
        return ok;
    };
    {
        beginCmd();
        beginPass(rpScene, fbScene, true, pColorMV, setScene);
        push(-0.5f, 0, 0.5f, 0.5f, kGreen);  // x = -0.5 + 0.5 * gl_InstanceIndex
        vkCmdDraw(g.cmd, 3, 3, kInstFirst, 0);
        vkCmdEndRenderPass(g.cmd);
        recordReadback(sceneColor, readback);
        submitAndWait();
        std::string detail;
        const bool ok = checkInstances(fetch("check5a_instance_index"), detail);
        report("5a", ok, "instanceCount 3, offset from gl_InstanceIndex" + detail);
    }
    {
        beginCmd();
        beginPass(rpScene, fbScene, true, pColorMVInst, setScene);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(g.cmd, 1, 1, &instBuf.buf, &off);
        push(0, 0, 0.5f, 0, kGreen);  // offsets come from the per-instance attribute
        vkCmdDraw(g.cmd, 3, 3, kInstFirst, 0);
        vkCmdEndRenderPass(g.cmd);
        recordReadback(sceneColor, readback);
        submitAndWait();
        std::string detail;
        const bool ok = checkInstances(fetch("check5b_instance_attr"), detail);
        report("5b", ok, "instanceCount 3, offset from a VK_VERTEX_INPUT_RATE_INSTANCE attribute" + detail);
    }

    // ---- 6: timing -------------------------------------------------------------------------------------------
    // Each draw: one vkCmdPushConstants (position, colour) + one vkCmdDraw of a tiny triangle, as DXVK would
    // issue per D3D9 draw. CPU = vkBegin..vkEndCommandBuffer (record) + vkQueueSubmit (MoltenVK encodes the
    // Metal commands here by default); wall = start of recording to fence signalled.
    {
        std::vector<PushConstants> pcs(draws);
        for (uint32_t i = 0; i < draws; ++i) {
            PushConstants& pc = pcs[i];
            memset(&pc, 0, sizeof pc);
            pc.xform[0] = -0.9f + (float)(i % 40) * 0.045f;
            pc.xform[1] = -0.9f + (float)((i / 40) % 40) * 0.045f;
            pc.xform[2] = 0.5f;
            pc.color[0] = (float)(i % 7) / 6.0f;
            pc.color[1] = (float)(i % 5) / 4.0f;
            pc.color[2] = (float)(i % 3) / 2.0f;
            pc.color[3] = 1.0f;
        }
        struct Case { const char* name; int mode; };  // 0 mono, 1 multiview, 2 two mono passes
        const Case cases[3] = {{"mono (1 layer)", 0}, {"multiview (2 layers)", 1}, {"two mono passes", 2}};
        double medRecord[3], medSubmit[3], medWall[3];
        for (int c = 0; c < 3; ++c) {
            std::vector<double> rec, sub, wall;
            for (uint32_t r = 0; r < reps + 1; ++r) {  // first run is a warm-up and is discarded
                VK_CHECK(vkResetCommandBuffer(g.cmd, 0));
                const auto t0 = std::chrono::steady_clock::now();
                VkCommandBufferBeginInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                VK_CHECK(vkBeginCommandBuffer(g.cmd, &bi));
                const int passes = cases[c].mode == 2 ? 2 : 1;
                for (int p = 0; p < passes; ++p) {
                    if (cases[c].mode == 0) beginPass(rpMono, fbMono, true, pColorMono, setZero);
                    else if (cases[c].mode == 1) beginPass(rpScene, fbScene, true, pColorMV, setZero);
                    else beginPass(rpMono, fbLayer[p], true, pColorMono, setZero);
                    for (uint32_t i = 0; i < draws; ++i) {
                        vkCmdPushConstants(g.cmd, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                           32, &pcs[i]);  // xform + colour
                        vkCmdDraw(g.cmd, 3, 1, kTinyFirst, 0);
                    }
                    vkCmdEndRenderPass(g.cmd);
                }
                VK_CHECK(vkEndCommandBuffer(g.cmd));
                const auto t1 = std::chrono::steady_clock::now();
                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &g.cmd;
                VK_CHECK(vkQueueSubmit(g.queue, 1, &si, g.fence));
                const auto t2 = std::chrono::steady_clock::now();
                VK_CHECK(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX));
                const auto t3 = std::chrono::steady_clock::now();
                VK_CHECK(vkResetFences(g.device, 1, &g.fence));
                if (r == 0) continue;
                rec.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                sub.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
                wall.push_back(std::chrono::duration<double, std::milli>(t3 - t0).count());
            }
            medRecord[c] = median(rec);
            medSubmit[c] = median(sub);
            medWall[c] = median(wall);
        }
        printf("[6] timing, %u draws per pass, %ux%u, median of %u (ms):\n", draws, W, H, reps);
        printf("       %-22s %10s %10s %10s %10s\n", "case", "record", "submit", "CPU total", "wall");
        for (int c = 0; c < 3; ++c)
            printf("       %-22s %10.3f %10.3f %10.3f %10.3f\n", cases[c].name, medRecord[c], medSubmit[c],
                   medRecord[c] + medSubmit[c], medWall[c]);
        const double cpuMono = medRecord[0] + medSubmit[0], cpuMV = medRecord[1] + medSubmit[1],
                     cpuTwo = medRecord[2] + medSubmit[2];
        printf("       CPU: multiview / mono = %.2fx, two passes / mono = %.2fx, multiview / two passes = %.2fx\n",
               cpuMV / cpuMono, cpuTwo / cpuMono, cpuMV / cpuTwo);
        printf("       wall: multiview / mono = %.2fx, two passes / mono = %.2fx, multiview / two passes = %.2fx\n",
               medWall[1] / medWall[0], medWall[2] / medWall[0], medWall[1] / medWall[2]);
        // Not a correctness check; it fails only if a case did not run at all.
        report("6", medWall[0] > 0 && medWall[1] > 0 && medWall[2] > 0, "timing completed (numbers above)");
    }

    // ---- 7: which Metal path ---------------------------------------------------------------------------------
    // MoltenVK uses one layered Metal pass (instance count x views, [[render_target_array_index]] from the
    // vertex shader) when the device supports layered rendering, else one Metal pass per view. Two hints:
    //   a) MVKPhysicalDeviceMetalFeatures::layeredRendering, from MoltenVK's own (deprecated but exported)
    //      vkGetPhysicalDeviceMetalFeaturesMVK, looked up with dlsym so nothing links against it;
    //   b) the MSL MoltenVK generated (MVK_CONFIG_SHADER_DUMP_DIR): render_target_array_index in the multiview
    //      vertex shader means the layered path.
    {
        int layered = -1;
        auto getMetalFeatures = (PFN_vkGetPhysicalDeviceMetalFeaturesMVK)vkGetInstanceProcAddr(g.instance, "vkGetPhysicalDeviceMetalFeaturesMVK");
        if (!getMetalFeatures) getMetalFeatures = (PFN_vkGetPhysicalDeviceMetalFeaturesMVK)dlsym(RTLD_DEFAULT, "vkGetPhysicalDeviceMetalFeaturesMVK");
        if (getMetalFeatures) {
            MVKPhysicalDeviceMetalFeatures mf;
            memset(&mf, 0, sizeof mf);
            size_t size = sizeof mf;
            VkResult r = getMetalFeatures(g.phys, &mf, &size);
            const size_t need = offsetof(MVKPhysicalDeviceMetalFeatures, layeredRendering) + sizeof(VkBool32);
            if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && size >= need) layered = mf.layeredRendering ? 1 : 0;
            printf("[7] vkGetPhysicalDeviceMetalFeaturesMVK: result %d, %zu bytes, layeredRendering=%d, MSL %u\n", (int)r,
                   size, layered, mf.mslVersion);
        } else {
            printf("[7] vkGetPhysicalDeviceMetalFeaturesMVK not found\n");
        }
        int filesTotal = 0, filesWithRTAI = 0, filesWithViewMask = 0;
        if (!shaderDumpDir.empty()) {
            std::vector<std::string> files;
            listFiles(shaderDumpDir, files);
            for (const std::string& f : files) {
                const std::string s = readFile(f);
                ++filesTotal;
                if (s.find("render_target_array_index") != std::string::npos) ++filesWithRTAI;
                if (s.find("spvViewMask") != std::string::npos) ++filesWithViewMask;
            }
            printf("[7] MSL dump %s: %d files, %d mention render_target_array_index, %d mention spvViewMask\n",
                   shaderDumpDir.c_str(), filesTotal, filesWithRTAI, filesWithViewMask);
        }
        const char* path;
        if (layered == 1 && filesWithRTAI > 0) path = "LAYERED: one Metal render pass, renderTargetArrayLength 2, instances x2 (confirmed by MSL)";
        else if (layered == 1) path = "LAYERED (from layeredRendering; MSL dump did not confirm, see README)";
        else if (layered == 0) path = "PER-VIEW: one Metal render pass per view (layeredRendering is false)";
        else if (filesWithRTAI > 0) path = "LAYERED (from MSL only)";
        else path = "UNDETERMINED (see README: confirm with a Metal capture)";
        printf("[7] INFO Metal path: %s\n", path);
    }

    // Orderly teardown. Besides tidiness this matters for MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE=1 (README), whose
    // Metal capture covers the VkDevice's lifetime and is only written out when the device is destroyed.
    VK_CHECK(vkDeviceWaitIdle(g.device));
    for (VkPipeline p : {pColorMV, pColorMVInst, pSampleMV, pColorMono}) vkDestroyPipeline(g.device, p, nullptr);
    for (VkShaderModule s : {vsMV, vsMVInst, vsMono, fsColor, fsSample}) vkDestroyShaderModule(g.device, s, nullptr);
    for (VkFramebuffer f : {fbScene, fbSample, fbMono, fbLayer[0], fbLayer[1]}) vkDestroyFramebuffer(g.device, f, nullptr);
    for (VkRenderPass r : {rpScene, rpSample, rpMono}) vkDestroyRenderPass(g.device, r, nullptr);
    vkDestroyDescriptorPool(g.device, dpool, nullptr);  // frees the sets
    vkDestroyPipelineLayout(g.device, playout, nullptr);
    vkDestroyDescriptorSetLayout(g.device, dsl, nullptr);
    vkDestroySampler(g.device, sampler, nullptr);
    for (VkImageView v : {sceneColorArray, sceneDepthArray, sceneColorLayer[0], sceneColorLayer[1], sceneDepthLayer[0],
                          sceneDepthLayer[1], sampleOutArray, monoColorView, monoDepthView, tex1Array})
        vkDestroyImageView(g.device, v, nullptr);
    for (const Image* im : {&sceneColor, &sceneDepth, &sampleOut, &monoColor, &monoDepth, &tex1}) {
        vkDestroyImage(g.device, im->img, nullptr);
        vkFreeMemory(g.device, im->mem, nullptr);
    }
    for (const Buffer* b : {&readback, &staging, &vbuf, &instBuf, &uboScene, &uboZero}) {
        vkDestroyBuffer(g.device, b->buf, nullptr);
        vkFreeMemory(g.device, b->mem, nullptr);  // implicitly unmaps
    }
    vkDestroyFence(g.device, g.fence, nullptr);
    vkDestroyCommandPool(g.device, g.pool, nullptr);
    vkDestroyDevice(g.device, nullptr);
    vkDestroyInstance(g.instance, nullptr);

    printf("\n%s: %d check(s) failed\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
