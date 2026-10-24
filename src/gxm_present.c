/* Direct GXM presentation. See gxm_present.h.
 *
 * Three display buffers, a clear to opaque black, one point-filtered textured
 * quad, and a display queue flip with no vblank wait. The frame pacer in
 * main_vita.c sets the rate.
 *
 * Memory: display buffers in CDRAM when it is free, main memory otherwise.
 * Everything the CPU writes each frame (textures, vertices) is uncached main
 * memory, because GPU reads of cached memory need a cache writeback that user
 * mode cannot request. */
#include "gxm_present.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/message_dialog.h>

#include "platform.h"
#include "shaders/gxm_shaders.h"

enum {
    kScreenW = 960,
    kScreenH = 544,
    kScreenStride = 960,
    kBuffers = 3,
    kMaxPending = kBuffers - 1,
    kTextures = 2,
    kGrSets = 2,
    kGrSamplers = 7,
};

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

typedef struct {
    void *address;
} DisplayData;

typedef struct {
    float x, y, u, v;
} QuadVertex;

typedef struct {
    float x, y;
} ClearVertex;

static struct {
    bool ready;
    SceGxmContext *context;
    SceGxmRenderTarget *target;
    SceGxmShaderPatcher *patcher;
    void *context_host;
    SceUID vdm_uid, vertex_ring_uid, fragment_ring_uid, fragment_usse_uid;
    SceUID patcher_buf_uid, patcher_vusse_uid, patcher_fusse_uid;
    SceUID display_uid[kBuffers], depth_uid, texture_uid[kTextures], vertices_uid;
    void *display[kBuffers];
    SceGxmColorSurface surface[kBuffers];
    SceGxmSyncObject *sync[kBuffers];
    SceGxmDepthStencilSurface depth;
    void *depth_mem;
    int back, front;
    SceGxmShaderPatcherId quad_v_id, quad_f_id, clear_v_id, clear_f_id;
    SceGxmVertexProgram *quad_vp, *clear_vp;
    SceGxmFragmentProgram *quad_fp, *clear_fp;
    SceGxmTexture texture[kTextures];
    uint32_t *texels[kTextures];
    /* Set by the GPU when the scene that read texture i has finished. */
    SceGxmNotification done[kTextures];
    int current;
    int tex_w, tex_h, tex_stride;
    int cap_w, cap_h; /* the size the texture memory was allocated for */
    QuadVertex *quad;
    ClearVertex *clear;
    uint16_t *indices;
    SceGxmRenderTarget *gr_target;
    SceGxmShaderPatcherId gr_f_id;
    SceGxmFragmentProgram *gr_fp;
    int gr_unit[kGrSamplers];
    SceUID gr_uid[kGrSets];
    GxmGroundSet gr_set[kGrSets];
    /* Per set: the shader's inputs, in kGrSamplers order, and its output as
     * a texture for the present scene. */
    SceGxmTexture gr_in[kGrSets][kGrSamplers];
    SceGxmTexture gr_out[kGrSets];
    SceGxmColorSurface gr_surface[kGrSets];
    SceGxmNotification gr_done[kGrSets];
    /* The flat frame, queried as integers by the fallback path. */
    SceGxmTexture flat_int[kTextures];
    /* The present notification slot and value that last read each set. */
    int gr_reader_slot[kGrSets];
    unsigned int gr_reader_value[kGrSets];
    int gr_current;
    bool gr_pending;
} g;

static void *GpuAlloc(SceKernelMemBlockType type, SceSize size,
                      SceGxmMemoryAttribFlags attribs, SceUID *uid) {
    size = ALIGN_UP(size, type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW
                              ? 256 * 1024 : 4 * 1024);
    *uid = sceKernelAllocMemBlock("fzero_gxm", type, size, NULL);
    if (*uid < 0) return NULL;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(*uid, &base) < 0 ||
        sceGxmMapMemory(base, size, attribs) < 0) {
        sceKernelFreeMemBlock(*uid);
        *uid = -1;
        return NULL;
    }
    return base;
}

static void GpuFree(SceUID uid) {
    if (uid < 0) return;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(uid, &base) >= 0) sceGxmUnmapMemory(base);
    sceKernelFreeMemBlock(uid);
}

static void *UsseAlloc(SceSize size, SceUID *uid, unsigned int *offset,
                       bool fragment) {
    size = ALIGN_UP(size, 4 * 1024);
    *uid = sceKernelAllocMemBlock("fzero_usse",
                                  SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                  size, NULL);
    if (*uid < 0) return NULL;
    void *base = NULL;
    sceKernelGetMemBlockBase(*uid, &base);
    int rc = fragment ? sceGxmMapFragmentUsseMemory(base, size, offset)
                      : sceGxmMapVertexUsseMemory(base, size, offset);
    if (rc < 0) {
        sceKernelFreeMemBlock(*uid);
        *uid = -1;
        return NULL;
    }
    return base;
}

static void UsseFree(SceUID uid, bool fragment) {
    if (uid < 0) return;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(uid, &base) >= 0) {
        if (fragment) sceGxmUnmapFragmentUsseMemory(base);
        else sceGxmUnmapVertexUsseMemory(base);
    }
    sceKernelFreeMemBlock(uid);
}

static void *PatcherHostAlloc(void *user, SceSize size) {
    (void)user;
    return malloc(size);
}

static void PatcherHostFree(void *user, void *mem) {
    (void)user;
    free(mem);
}

static void DisplayCallback(const void *data) {
    const DisplayData *d = data;
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = d->address;
    fb.pitch = kScreenStride;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = kScreenW;
    fb.height = kScreenH;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

#define CHECK(expr)                                                        \
    do {                                                                   \
        int rc_ = (int)(expr);                                             \
        if (rc_ < 0) {                                                     \
            FZERO_LOG("ERROR", "gxm: %s failed: %#x", #expr, (unsigned)rc_); \
            return false;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_PTR(ptr, what)                                               \
    do {                                                                   \
        if (!(ptr)) {                                                      \
            FZERO_LOG("ERROR", "gxm: %s failed", what);                    \
            return false;                                                  \
        }                                                                  \
    } while (0)

static bool InitContext(void) {
    SceGxmInitializeParams ip;
    memset(&ip, 0, sizeof(ip));
    ip.displayQueueMaxPendingCount = kMaxPending;
    ip.displayQueueCallback = DisplayCallback;
    ip.displayQueueCallbackDataSize = sizeof(DisplayData);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    CHECK(sceGxmInitialize(&ip));

    const SceKernelMemBlockType nc = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    void *vdm = GpuAlloc(nc, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,
                         SCE_GXM_MEMORY_ATTRIB_READ, &g.vdm_uid);
    void *vring = GpuAlloc(nc, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE,
                           SCE_GXM_MEMORY_ATTRIB_READ, &g.vertex_ring_uid);
    void *fring = GpuAlloc(nc, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE,
                           SCE_GXM_MEMORY_ATTRIB_READ, &g.fragment_ring_uid);
    unsigned int fusse_offset = 0;
    void *fusse = UsseAlloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
                            &g.fragment_usse_uid, &fusse_offset, true);
    CHECK_PTR(vdm && vring && fring && fusse, "ring buffer allocation");

    g.context_host = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    CHECK_PTR(g.context_host, "context host memory");
    SceGxmContextParams cp;
    memset(&cp, 0, sizeof(cp));
    cp.hostMem = g.context_host;
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = vdm;
    cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = vring;
    cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = fring;
    cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = fusse;
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = fusse_offset;
    CHECK(sceGxmCreateContext(&cp, &g.context));

    SceGxmRenderTargetParams rp;
    memset(&rp, 0, sizeof(rp));
    rp.width = kScreenW;
    rp.height = kScreenH;
    rp.scenesPerFrame = 1;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.driverMemBlock = -1;
    CHECK(sceGxmCreateRenderTarget(&rp, &g.target));
    return true;
}

static bool InitSurfaces(void) {
    const SceSize fb_size = 4 * kScreenStride * kScreenH;
    int in_cdram = 0;
    for (int i = 0; i < kBuffers; ++i) {
        g.display[i] = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, fb_size,
                                SCE_GXM_MEMORY_ATTRIB_RW, &g.display_uid[i]);
        if (g.display[i]) {
            ++in_cdram;
        } else {
            g.display[i] = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, fb_size,
                                    SCE_GXM_MEMORY_ATTRIB_RW, &g.display_uid[i]);
        }
        CHECK_PTR(g.display[i], "display buffer allocation");
        memset(g.display[i], 0, fb_size);
        CHECK(sceGxmColorSurfaceInit(&g.surface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                     SCE_GXM_COLOR_SURFACE_LINEAR,
                                     SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                     SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                                     kScreenW, kScreenH, kScreenStride,
                                     g.display[i]));
        CHECK(sceGxmSyncObjectCreate(&g.sync[i]));
    }
    FZERO_LOG("INFO", "gxm: %d display buffers, %d in CDRAM", kBuffers, in_cdram);

    const unsigned int dw = ALIGN_UP(kScreenW, SCE_GXM_TILE_SIZEX);
    const unsigned int dh = ALIGN_UP(kScreenH, SCE_GXM_TILE_SIZEY);
    void *depth = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * dw * dh,
                           SCE_GXM_MEMORY_ATTRIB_RW, &g.depth_uid);
    CHECK_PTR(depth, "depth buffer allocation");
    g.depth_mem = depth;
    CHECK(sceGxmDepthStencilSurfaceInit(&g.depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                        SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                        dw, depth, NULL));
    g.front = 0;
    g.back = 1;
    return true;
}

static bool FindAttribute(const SceGxmProgram *program, const char *name,
                          SceGxmVertexAttribute *attr, unsigned short offset) {
    const SceGxmProgramParameter *p = sceGxmProgramFindParameterByName(program, name);
    if (!p) {
        FZERO_LOG("ERROR", "gxm: vertex attribute %s not found", name);
        return false;
    }
    attr->streamIndex = 0;
    attr->offset = offset;
    attr->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr->componentCount = 2;
    attr->regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(p);
    return true;
}

static bool InitShaders(void) {
    const SceSize buf_size = 64 * 1024, usse_size = 64 * 1024;
    void *buf = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, buf_size,
                         SCE_GXM_MEMORY_ATTRIB_READ, &g.patcher_buf_uid);
    unsigned int vusse_offset = 0, fusse_offset = 0;
    void *vusse = UsseAlloc(usse_size, &g.patcher_vusse_uid, &vusse_offset, false);
    void *fusse = UsseAlloc(usse_size, &g.patcher_fusse_uid, &fusse_offset, true);
    CHECK_PTR(buf && vusse && fusse, "shader patcher memory");

    SceGxmShaderPatcherParams pp;
    memset(&pp, 0, sizeof(pp));
    pp.hostAllocCallback = PatcherHostAlloc;
    pp.hostFreeCallback = PatcherHostFree;
    pp.bufferMem = buf;
    pp.bufferMemSize = buf_size;
    pp.vertexUsseMem = vusse;
    pp.vertexUsseMemSize = usse_size;
    pp.vertexUsseOffset = vusse_offset;
    pp.fragmentUsseMem = fusse;
    pp.fragmentUsseMemSize = usse_size;
    pp.fragmentUsseOffset = fusse_offset;
    CHECK(sceGxmShaderPatcherCreate(&pp, &g.patcher));

    const SceGxmProgram *quad_v = (const SceGxmProgram *)g_gxp_quad_v;
    const SceGxmProgram *quad_f = (const SceGxmProgram *)g_gxp_quad_f;
    const SceGxmProgram *clear_v = (const SceGxmProgram *)g_gxp_clear_v;
    const SceGxmProgram *clear_f = (const SceGxmProgram *)g_gxp_clear_f;
    CHECK(sceGxmProgramCheck(quad_v));
    CHECK(sceGxmProgramCheck(quad_f));
    CHECK(sceGxmProgramCheck(clear_v));
    CHECK(sceGxmProgramCheck(clear_f));
    CHECK(sceGxmShaderPatcherRegisterProgram(g.patcher, quad_v, &g.quad_v_id));
    CHECK(sceGxmShaderPatcherRegisterProgram(g.patcher, quad_f, &g.quad_f_id));
    CHECK(sceGxmShaderPatcherRegisterProgram(g.patcher, clear_v, &g.clear_v_id));
    CHECK(sceGxmShaderPatcherRegisterProgram(g.patcher, clear_f, &g.clear_f_id));

    SceGxmVertexAttribute quad_attr[2];
    if (!FindAttribute(quad_v, "aPosition", &quad_attr[0], 0) ||
        !FindAttribute(quad_v, "aTexcoord", &quad_attr[1], 8))
        return false;
    SceGxmVertexStream quad_stream = { sizeof(QuadVertex),
                                       SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    CHECK(sceGxmShaderPatcherCreateVertexProgram(g.patcher, g.quad_v_id, quad_attr, 2,
                                                 &quad_stream, 1, &g.quad_vp));
    CHECK(sceGxmShaderPatcherCreateFragmentProgram(g.patcher, g.quad_f_id,
                                                   SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                   SCE_GXM_MULTISAMPLE_NONE, NULL,
                                                   quad_v, &g.quad_fp));

    SceGxmVertexAttribute clear_attr;
    if (!FindAttribute(clear_v, "aPosition", &clear_attr, 0)) return false;
    SceGxmVertexStream clear_stream = { sizeof(ClearVertex),
                                        SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    CHECK(sceGxmShaderPatcherCreateVertexProgram(g.patcher, g.clear_v_id, &clear_attr, 1,
                                                 &clear_stream, 1, &g.clear_vp));
    CHECK(sceGxmShaderPatcherCreateFragmentProgram(g.patcher, g.clear_f_id,
                                                   SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                   SCE_GXM_MULTISAMPLE_NONE, NULL,
                                                   clear_v, &g.clear_fp));
    return true;
}

/* Describe both textures as src_w x src_h. Their memory holds cap_w x cap_h. */
static bool SetTextureSize(int src_w, int src_h) {
    /* Linear textures need a row stride of a multiple of 8 texels. The wide
     * frame is 398 wide, so rows are copied into a 400-texel stride. */
    if (src_w > g.cap_w || src_h > g.cap_h) {
        FZERO_LOG("ERROR", "gxm: frame %dx%d is larger than the texture %dx%d",
                  src_w, src_h, g.cap_w, g.cap_h);
        return false;
    }
    g.tex_w = src_w;
    g.tex_h = src_h;
    g.tex_stride = ALIGN_UP(src_w, 8);
    for (int i = 0; i < kTextures; ++i) {
        memset(g.texels[i], 0, 4 * (size_t)ALIGN_UP(g.cap_w, 8) * g.cap_h);
        CHECK(sceGxmTextureInitLinearStrided(&g.texture[i], g.texels[i],
                                             SCE_GXM_TEXTURE_FORMAT_A8R8G8B8,
                                             src_w, src_h, 4 * g.tex_stride));
        sceGxmTextureSetMinFilter(&g.texture[i], SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&g.texture[i], SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&g.texture[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&g.texture[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
    }
    return true;
}

static bool InitTextureAndGeometry(int src_w, int src_h) {
    g.cap_w = src_w;
    g.cap_h = src_h;
    const SceSize tex_size = 4 * ALIGN_UP(src_w, 8) * src_h;
    volatile unsigned int *region = sceGxmGetNotificationRegion();
    for (int i = 0; i < kTextures; ++i) {
        g.texels[i] = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                               tex_size, SCE_GXM_MEMORY_ATTRIB_READ,
                               &g.texture_uid[i]);
        CHECK_PTR(g.texels[i], "texture allocation");
        g.done[i].address = &region[i];
        g.done[i].value = 0;
        region[i] = 0;
    }
    g.current = 0;
    if (!SetTextureSize(src_w, src_h)) return false;

    /* One block for both quads and the shared index list. The quad's
     * positions are rewritten only when the destination rectangle changes. */
    const SceSize size = sizeof(QuadVertex) * 4 + sizeof(ClearVertex) * 4 +
                         sizeof(uint16_t) * 4;
    uint8_t *mem = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size,
                            SCE_GXM_MEMORY_ATTRIB_READ, &g.vertices_uid);
    CHECK_PTR(mem, "vertex allocation");
    g.quad = (QuadVertex *)mem;
    g.clear = (ClearVertex *)(mem + sizeof(QuadVertex) * 4);
    g.indices = (uint16_t *)(mem + sizeof(QuadVertex) * 4 + sizeof(ClearVertex) * 4);

    static const ClearVertex clear[4] = { {-1, 1}, {1, 1}, {-1, -1}, {1, -1} };
    memcpy(g.clear, clear, sizeof(clear));
    for (uint16_t i = 0; i < 4; ++i) g.indices[i] = i;
    return true;
}

static bool GrTexture(SceGxmTexture *t, const void *data, SceGxmTextureFormat format,
                      int w, int h) {
    if (sceGxmTextureInitLinear(t, data, format, w, h, 0) < 0) {
        FZERO_LOG("ERROR", "gxm: ground texture %dx%d fmt %#x failed", w, h, (unsigned)format);
        return false;
    }
    sceGxmTextureSetMinFilter(t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(t, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(t, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(t, SCE_GXM_TEXTURE_ADDR_CLAMP);
    return true;
}

static const char *const kGrSamplerNames[kGrSamplers] = {
    "lines", "mtiles", "vram", "objz", "cgram", "course", "flat",
};

static bool InitGround(void) {
    const SceGxmProgram *gr_f = (const SceGxmProgram *)g_gxp_ground_f;
    CHECK(sceGxmProgramCheck(gr_f));
    CHECK(sceGxmShaderPatcherRegisterProgram(g.patcher, gr_f, &g.gr_f_id));
    /* The clear pass's vertex program already draws a full-target quad. */
    CHECK(sceGxmShaderPatcherCreateFragmentProgram(g.patcher, g.gr_f_id,
                                                   SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                   SCE_GXM_MULTISAMPLE_NONE, NULL,
                                                   (const SceGxmProgram *)g_gxp_clear_v,
                                                   &g.gr_fp));
    for (int k = 0; k < kGrSamplers; ++k) {
        const SceGxmProgramParameter *p =
            sceGxmProgramFindParameterByName(gr_f, kGrSamplerNames[k]);
        CHECK_PTR(p, kGrSamplerNames[k]);
        g.gr_unit[k] = (int)sceGxmProgramParameterGetResourceIndex(p);
    }

    SceGxmRenderTargetParams rp;
    memset(&rp, 0, sizeof(rp));
    rp.width = kGxmGroundW;
    rp.height = kGxmGroundH;
    rp.scenesPerFrame = 1;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.driverMemBlock = -1;
    CHECK(sceGxmCreateRenderTarget(&rp, &g.gr_target));

    const SceSize out = 4 * kGxmGroundStride * kGxmGroundH;
    const SceSize lines = 8 * kGxmGroundLineTexels * kGxmGroundH;
    const SceSize mtiles = kGxmGroundTilesW * kGxmGroundH;
    const SceSize vram = 2 * 128 * 128;
    const SceSize objz = 2 * kGxmGroundStride * kGxmGroundH;
    const SceSize cgram = 4 * 256;
    const SceSize course = 256 * 256;
    const SceSize total = out + lines + mtiles + vram + objz + cgram + course;
    volatile unsigned int *region = sceGxmGetNotificationRegion();
    for (int i = 0; i < kGrSets; ++i) {
        uint8_t *mem = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, total,
                                SCE_GXM_MEMORY_ATTRIB_RW, &g.gr_uid[i]);
        CHECK_PTR(mem, "ground allocation");
        memset(mem, 0, total);
        GxmGroundSet *set = &g.gr_set[i];
        set->out = (const uint32_t *)mem;
        set->lines = (uint16_t *)(mem + out);
        set->mtiles = mem + out + lines;
        set->vram = (uint16_t *)(mem + out + lines + mtiles);
        set->objz = (uint16_t *)(mem + out + lines + mtiles + vram);
        set->cgram = mem + out + lines + mtiles + vram + objz;
        set->course = set->cgram + cgram;
        SceGxmTexture *in = g.gr_in[i];
        if (!GrTexture(&in[0], set->lines, SCE_GXM_TEXTURE_FORMAT_U16U16U16U16_ABGR,
                       kGxmGroundLineTexels, kGxmGroundH) ||
            !GrTexture(&in[1], set->mtiles, SCE_GXM_TEXTURE_FORMAT_U8_R,
                       kGxmGroundTilesW, kGxmGroundH) ||
            !GrTexture(&in[2], set->vram, SCE_GXM_TEXTURE_FORMAT_U8U8_GR, 128, 128) ||
            !GrTexture(&in[3], set->objz, SCE_GXM_TEXTURE_FORMAT_U16_R,
                       kGxmGroundW, kGxmGroundH) ||
            !GrTexture(&in[4], set->cgram, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 256, 1) ||
            !GrTexture(&in[5], set->course, SCE_GXM_TEXTURE_FORMAT_U8_R, 256, 256) ||
            !GrTexture(&g.gr_out[i], mem, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                       kGxmGroundW, kGxmGroundH))
            return false;
        CHECK(sceGxmColorSurfaceInit(&g.gr_surface[i], SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
                                     SCE_GXM_COLOR_SURFACE_LINEAR,
                                     SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                     SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
                                     kGxmGroundW, kGxmGroundH, kGxmGroundStride, mem));
        /* Slots 0 and 1 belong to the present textures. */
        g.gr_done[i].address = &region[2 + i];
        g.gr_done[i].value = 0;
        region[2 + i] = 0;
        g.gr_reader_slot[i] = -1;
    }
    for (int i = 0; i < kTextures; ++i)
        if (!GrTexture(&g.flat_int[i], g.texels[i], SCE_GXM_TEXTURE_FORMAT_A8R8G8B8,
                       g.tex_w, g.tex_h))
            return false;
    g.gr_current = 0;
    FZERO_LOG("INFO", "gxm: ground ready, %dx%d", kGxmGroundW, kGxmGroundH);
    return true;
}

bool GxmGroundAcquire(GxmGroundSet *set) {
    if (!g.ready || !g.gr_fp) return false;
    g.gr_current ^= 1;
    const int i = g.gr_current;
    sceGxmNotificationWait(&g.gr_done[i]);
    /* The present scene that drew this set's output. Its slot has moved on
     * by now if a later present used it, so wait for "at least". */
    if (g.gr_reader_slot[i] >= 0) {
        volatile unsigned int *slot = g.done[g.gr_reader_slot[i]].address;
        while ((int)(*slot - g.gr_reader_value[i]) < 0)
            sceKernelDelayThread(50);
    }
    *set = g.gr_set[i];
    return true;
}

uint32_t GxmGroundSubmit(bool sync) {
    if (!g.ready || !g.gr_fp) return 0;
    const int i = g.gr_current;
    sceGxmBeginScene(g.context, 0, g.gr_target, NULL, NULL, NULL,
                     &g.gr_surface[i], NULL);
    sceGxmSetVertexProgram(g.context, g.clear_vp);
    sceGxmSetFragmentProgram(g.context, g.gr_fp);
    for (int k = 0; k < kGrSamplers - 1; ++k)
        sceGxmSetFragmentTexture(g.context, g.gr_unit[k], &g.gr_in[i][k]);
    /* The frame GxmPresentUpload just wrote. */
    sceGxmSetFragmentTexture(g.context, g.gr_unit[kGrSamplers - 1], &g.flat_int[g.current]);
    sceGxmSetVertexStream(g.context, 0, g.clear);
    sceGxmDraw(g.context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
               SCE_GXM_INDEX_FORMAT_U16, g.indices, 4);
    g.gr_done[i].value++;
    sceGxmEndScene(g.context, NULL, &g.gr_done[i]);
    g.gr_pending = true;
    if (!sync) return 0;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    sceGxmNotificationWait(&g.gr_done[i]);
    return (uint32_t)(sceKernelGetProcessTimeWide() - t0);
}

bool GxmPresentInit(int src_w, int src_h) {
    memset(&g, 0, sizeof(g));
    g.vdm_uid = g.vertex_ring_uid = g.fragment_ring_uid = g.fragment_usse_uid = -1;
    g.patcher_buf_uid = g.patcher_vusse_uid = g.patcher_fusse_uid = -1;
    g.depth_uid = g.vertices_uid = -1;
    for (int i = 0; i < kTextures; ++i) g.texture_uid[i] = -1;
    for (int i = 0; i < kBuffers; ++i) g.display_uid[i] = -1;
    for (int i = 0; i < kGrSets; ++i) g.gr_uid[i] = -1;
    if (!InitContext() || !InitSurfaces() || !InitShaders() ||
        !InitTextureAndGeometry(src_w, src_h))
        return false;
    if (!InitGround()) return false;
    g.ready = true;
    FZERO_LOG("INFO", "gxm: present ready, texture %dx%d stride %d",
              src_w, src_h, g.tex_stride);
    return true;
}

bool GxmPresentSetSourceSize(int src_w, int src_h) {
    if (!g.ready) return false;
    /* Both textures may still be read by a queued scene. */
    sceGxmFinish(g.context);
    if (!SetTextureSize(src_w, src_h)) return false;
    FZERO_LOG("INFO", "gxm: texture now %dx%d stride %d", src_w, src_h, g.tex_stride);
    return true;
}

/* One frame of a common dialog: clear to black, let the system draw the
 * dialog over it, and flip. */
static void DialogFrame(void) {
    sceGxmBeginScene(g.context, 0, g.target, NULL, NULL, g.sync[g.back],
                     &g.surface[g.back], &g.depth);
    sceGxmSetFrontDepthFunc(g.context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g.context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetVertexProgram(g.context, g.clear_vp);
    sceGxmSetFragmentProgram(g.context, g.clear_fp);
    sceGxmSetVertexStream(g.context, 0, g.clear);
    sceGxmDraw(g.context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
               SCE_GXM_INDEX_FORMAT_U16, g.indices, 4);
    sceGxmEndScene(g.context, NULL, NULL);

    SceCommonDialogUpdateParam up;
    memset(&up, 0, sizeof(up));
    up.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    up.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    up.renderTarget.width = kScreenW;
    up.renderTarget.height = kScreenH;
    up.renderTarget.strideInPixels = kScreenStride;
    up.renderTarget.colorSurfaceData = g.display[g.back];
    up.renderTarget.depthSurfaceData = g.depth_mem;
    up.displaySyncObject = g.sync[g.back];
    sceCommonDialogUpdate(&up);

    sceGxmPadHeartbeat(&g.surface[g.back], g.sync[g.back]);
    DisplayData d = { g.display[g.back] };
    sceGxmDisplayQueueAddEntry(g.sync[g.front], g.sync[g.back], &d);
    g.front = g.back;
    g.back = (g.back + 1) % kBuffers;
    sceDisplayWaitVblankStart();
}

int GxmPresentAskYesNo(const char *message) {
    if (!g.ready) return -1;
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot); /* fails harmlessly if SDL already did it */

    SceMsgDialogUserMessageParam msg;
    memset(&msg, 0, sizeof(msg));
    msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_YESNO;
    msg.msg = (const SceChar8 *)message;
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;
    int rc = sceMsgDialogInit(&param);
    if (rc < 0) {
        FZERO_LOG("ERROR", "gxm: sceMsgDialogInit failed: %#x", (unsigned)rc);
        return -1;
    }
    while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
        DialogFrame();

    SceMsgDialogResult result;
    memset(&result, 0, sizeof(result));
    sceMsgDialogGetResult(&result);
    sceMsgDialogTerm();
    /* Two more frames, so the last one shown is not the dialog. */
    DialogFrame();
    DialogFrame();
    sceGxmFinish(g.context);
    if (result.result != SCE_COMMON_DIALOG_RESULT_OK) return -1;
    return result.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES ? 1 : 0;
}

void GxmPresentUpload(const uint32_t *pixels, int src_stride) {
    if (!g.ready) return;
    /* Two textures alternate. Wait only for the scene that last read the one
     * about to be written, normally finished a frame ago; a full
     * sceGxmFinish here would drain the whole GPU every frame. */
    g.current ^= 1;
    sceGxmNotificationWait(&g.done[g.current]);
    uint32_t *dst = g.texels[g.current];
    if (src_stride == g.tex_stride) {
        memcpy(dst, pixels, 4 * (size_t)g.tex_stride * g.tex_h);
        return;
    }
    for (int y = 0; y < g.tex_h; ++y)
        memcpy(dst + (size_t)y * g.tex_stride,
               pixels + (size_t)y * src_stride, 4 * (size_t)g.tex_w);
}

void GxmPresentDraw(int x, int y, int w, int h) {
    if (!g.ready) return;
    const float x0 = (float)x / (kScreenW / 2) - 1.0f;
    const float x1 = (float)(x + w) / (kScreenW / 2) - 1.0f;
    const float y0 = 1.0f - (float)y / (kScreenH / 2);
    const float y1 = 1.0f - (float)(y + h) / (kScreenH / 2);
    if (g.quad[0].x != x0 || g.quad[0].y != y0 ||
        g.quad[3].x != x1 || g.quad[3].y != y1) {
        const QuadVertex q[4] = {
            { x0, y0, 0.0f, 0.0f }, { x1, y0, 1.0f, 0.0f },
            { x0, y1, 0.0f, 1.0f }, { x1, y1, 1.0f, 1.0f },
        };
        memcpy(g.quad, q, sizeof(q));
    }

    sceGxmBeginScene(g.context, 0, g.target, NULL, NULL, g.sync[g.back],
                     &g.surface[g.back], &g.depth);
    sceGxmSetFrontDepthFunc(g.context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g.context, SCE_GXM_DEPTH_WRITE_DISABLED);

    sceGxmSetVertexProgram(g.context, g.clear_vp);
    sceGxmSetFragmentProgram(g.context, g.clear_fp);
    sceGxmSetVertexStream(g.context, 0, g.clear);
    sceGxmDraw(g.context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
               SCE_GXM_INDEX_FORMAT_U16, g.indices, 4);

    sceGxmSetVertexProgram(g.context, g.quad_vp);
    sceGxmSetFragmentProgram(g.context, g.quad_fp);
    /* A frame with ground lines is shown from the GPU's composite, which
     * already carries the CPU's pixels on every line it did not draw. */
    const bool ground = g.gr_pending;
    if (ground) {
        g.gr_reader_slot[g.gr_current] = g.current;
        g.gr_reader_value[g.gr_current] = g.done[g.current].value + 1;
        g.gr_pending = false;
    }
    sceGxmSetFragmentTexture(g.context, 0,
                             ground ? &g.gr_out[g.gr_current] : &g.texture[g.current]);
    sceGxmSetVertexStream(g.context, 0, g.quad);
    sceGxmDraw(g.context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP,
               SCE_GXM_INDEX_FORMAT_U16, g.indices, 4);
    g.done[g.current].value++;
    sceGxmEndScene(g.context, NULL, &g.done[g.current]);

    sceGxmPadHeartbeat(&g.surface[g.back], g.sync[g.back]);
    DisplayData d = { g.display[g.back] };
    sceGxmDisplayQueueAddEntry(g.sync[g.front], g.sync[g.back], &d);
    g.front = g.back;
    g.back = (g.back + 1) % kBuffers;
}

void GxmPresentShutdown(void) {
    if (!g.context) return;
    sceGxmFinish(g.context);
    sceGxmDisplayQueueFinish();
    if (g.patcher) {
        if (g.quad_vp) sceGxmShaderPatcherReleaseVertexProgram(g.patcher, g.quad_vp);
        if (g.quad_fp) sceGxmShaderPatcherReleaseFragmentProgram(g.patcher, g.quad_fp);
        if (g.clear_vp) sceGxmShaderPatcherReleaseVertexProgram(g.patcher, g.clear_vp);
        if (g.clear_fp) sceGxmShaderPatcherReleaseFragmentProgram(g.patcher, g.clear_fp);
        if (g.gr_fp) sceGxmShaderPatcherReleaseFragmentProgram(g.patcher, g.gr_fp);
        sceGxmShaderPatcherDestroy(g.patcher);
    }
    GpuFree(g.vertices_uid);
    for (int i = 0; i < kGrSets; ++i) GpuFree(g.gr_uid[i]);
    if (g.gr_target) sceGxmDestroyRenderTarget(g.gr_target);
    for (int i = 0; i < kTextures; ++i) GpuFree(g.texture_uid[i]);
    GpuFree(g.depth_uid);
    for (int i = 0; i < kBuffers; ++i) {
        if (g.sync[i]) sceGxmSyncObjectDestroy(g.sync[i]);
        GpuFree(g.display_uid[i]);
    }
    if (g.target) sceGxmDestroyRenderTarget(g.target);
    sceGxmDestroyContext(g.context);
    GpuFree(g.patcher_buf_uid);
    UsseFree(g.patcher_vusse_uid, false);
    UsseFree(g.patcher_fusse_uid, true);
    GpuFree(g.vdm_uid);
    GpuFree(g.vertex_ring_uid);
    GpuFree(g.fragment_ring_uid);
    UsseFree(g.fragment_usse_uid, true);
    free(g.context_host);
    sceGxmTerminate();
    memset(&g, 0, sizeof(g));
}
