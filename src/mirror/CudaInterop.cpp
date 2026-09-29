#include "CudaInterop.h"

extern "C" {
#include <ffnvcodec/dynlink_loader.h>
#include <libavutil/frame.h>
}

namespace remora::mirror {

// One driver-API table per process. Never freed: the registrations it serves live as long as the
// GL context, i.e. as long as the window.
static CudaFunctions *cu = nullptr;

bool CudaInterop::available() {
    static const bool ok = cuda_load_functions(&cu, nullptr) == 0;
    return ok;
}

static QString cuError(const char *what, CUresult rc) {
    const char *name = nullptr;
    if (cu->cuGetErrorName) cu->cuGetErrorName(rc, &name);
    return QStringLiteral("%1: %2").arg(QLatin1String(what),
                                        name ? QLatin1String(name) : QString::number(int(rc)));
}

// Every CUDA call runs under the DECODER's context — the frames and the registrations belong to
// it, and paintGL's thread has no CUDA context of its own current.
namespace {
struct CtxGuard {
    explicit CtxGuard(void *ctx) { cu->cuCtxPushCurrent(static_cast<CUcontext>(ctx)); }
    ~CtxGuard() {
        CUcontext dummy;
        cu->cuCtxPopCurrent(&dummy);
    }
};
}  // namespace

bool CudaInterop::registerTextures(unsigned texY, unsigned texUV, void *cudaCtx, QString *why) {
    if (!available() || !cudaCtx) {
        if (why) *why = QStringLiteral("libcuda or the decoder's CUDA context is missing");
        return false;
    }
    unregister();
    ctx_ = cudaCtx;
    CtxGuard guard(ctx_);
    constexpr unsigned kGlTexture2D = 0x0DE1;  // GL_TEXTURE_2D; dynlink_cuda.h has no GL enums
    const unsigned tex[2] = {texY, texUV};
    for (int i = 0; i < 2; ++i) {
        // WRITE_DISCARD: every map overwrites the whole plane, so the driver never needs to
        // preserve or migrate the previous contents.
        const CUresult rc = cu->cuGraphicsGLRegisterImage(
            reinterpret_cast<CUgraphicsResource *>(&res_[i]), tex[i], kGlTexture2D,
            CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD);
        if (rc != CUDA_SUCCESS) {
            if (why) *why = cuError("cuGraphicsGLRegisterImage", rc);
            unregister();
            return false;
        }
    }
    return true;
}

bool CudaInterop::upload(const AVFrame *f, QString *why) {
    if (!res_[0] || !res_[1]) {
        if (why) *why = QStringLiteral("textures are not registered");
        return false;
    }
    CtxGuard guard(ctx_);
    CUgraphicsResource res[2] = {static_cast<CUgraphicsResource>(res_[0]),
                                 static_cast<CUgraphicsResource>(res_[1])};
    CUresult rc = cu->cuGraphicsMapResources(2, res, nullptr);
    if (rc != CUDA_SUCCESS) {
        if (why) *why = cuError("cuGraphicsMapResources", rc);
        return false;
    }
    // NV12: plane 0 is one byte per pixel, plane 1 interleaved UV at half resolution in both
    // axes — two bytes per sample, so both planes copy `width` bytes per row.
    const size_t widths[2] = {size_t(f->width), size_t(f->width / 2) * 2};
    const size_t heights[2] = {size_t(f->height), size_t(f->height / 2)};
    for (int i = 0; i < 2 && rc == CUDA_SUCCESS; ++i) {
        CUarray arr;
        rc = cu->cuGraphicsSubResourceGetMappedArray(&arr, res[i], 0, 0);
        if (rc != CUDA_SUCCESS) {
            if (why) *why = cuError("cuGraphicsSubResourceGetMappedArray", rc);
            break;
        }
        CUDA_MEMCPY2D copy = {};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = reinterpret_cast<CUdeviceptr>(f->data[i]);
        copy.srcPitch = size_t(f->linesize[i]);
        copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.dstArray = arr;
        copy.WidthInBytes = widths[i];
        copy.Height = heights[i];
        rc = cu->cuMemcpy2D(&copy);
        if (rc != CUDA_SUCCESS && why) *why = cuError("cuMemcpy2D", rc);
    }
    cu->cuGraphicsUnmapResources(2, res, nullptr);
    return rc == CUDA_SUCCESS;
}

void CudaInterop::unregister() {
    if (!cu) return;
    if (res_[0] || res_[1]) {
        CtxGuard guard(ctx_);
        for (void *&r : res_) {
            if (r) cu->cuGraphicsUnregisterResource(static_cast<CUgraphicsResource>(r));
            r = nullptr;
        }
    }
    ctx_ = nullptr;
}

CudaInterop::~CudaInterop() { unregister(); }

}  // namespace remora::mirror
