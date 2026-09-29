#pragma once
#include <QString>

struct AVFrame;

namespace remora::mirror {

// CUDA/GL interop for the zero-copy decode-to-texture path (bd remora-28ix.2.3): NVDEC's decoded
// NV12 surface is copied device-to-device into the window's two plane textures, so the frame
// never crosses PCIe. The GPU->CPU download it replaces measured ~32 ms/frame at 3760x1992 —
// the whole gap between the ~30 fps the window drew and the 58-60 the stream delivered.
//
// libcuda is loaded at runtime through the ffnvcodec dynlink headers (the same mechanism FFmpeg
// itself uses), so there is no build- or link-time CUDA dependency: on a host without libcuda
// available() is false and the caller keeps the download path.
class CudaInterop {
public:
    CudaInterop() = default;
    CudaInterop(const CudaInterop &) = delete;
    CudaInterop &operator=(const CudaInterop &) = delete;
    ~CudaInterop();

    // Whether libcuda loaded (once per process). Says nothing about whether the GL context can
    // interop — that verdict is registerTextures()'s, per context.
    static bool available();

    // (Re)register the NV12 plane textures with CUDA. The GL context they belong to must be
    // current, and their storage must already be allocated at the streamed size — registration
    // binds to the texture's current storage, so a glTexImage2D realloc needs a re-register.
    // cudaCtx is the decoder device's CUcontext (Decoder::cudaContext()). Fails cleanly when the
    // GL context lives on a different GPU than the decoder — the cross-GPU split VAAPI dies on.
    bool registerTextures(unsigned texY, unsigned texUV, void *cudaCtx, QString *why);

    // Copy one decoded CUDA NV12 frame into the registered textures, device-to-device.
    bool upload(const AVFrame *f, QString *why);

    // Drop the registrations (idempotent). Called on re-register, failure, and destruction.
    void unregister();

private:
    void *ctx_ = nullptr;                 // CUcontext the registrations belong to
    void *res_[2] = {nullptr, nullptr};   // CUgraphicsResource per plane (Y, UV)
};

}  // namespace remora::mirror
