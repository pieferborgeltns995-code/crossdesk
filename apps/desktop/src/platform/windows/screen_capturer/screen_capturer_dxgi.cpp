#include "screen_capturer_dxgi.h"

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <display_stream_id.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "captured_cursor_state.h"
#include "cursor_draw.h"
#include "cursor_frame_compositor.h"
#include "display_label.h"
#include "dxgi_cursor_state.h"
#include "libyuv.h"
#include "rd_log.h"
#include "windows_thread_dpi.h"
#include "../virtual_display/usbmmidd_virtual_display.h"

namespace crossdesk {

namespace {
std::wstring GetOutputDeviceId(const std::wstring& device_name) {
  DISPLAY_DEVICEW device{sizeof(device)};
  if (!EnumDisplayDevicesW(device_name.c_str(), 0, &device,
                           EDD_GET_DEVICE_INTERFACE_NAME)) {
    return {};
  }
  return device.DeviceID;
}

uint32_t AlignUpDim(uint32_t value, uint32_t align) {
  return ((value + align - 1U) / align) * align;
}

void ComputeAlignedNv12TextureSize(uint32_t even_w, uint32_t even_h,
                                     uint32_t* tex_w, uint32_t* tex_h) {
  if (even_w == 1920U && even_h == 1080U) {
    *tex_w = 1920U;
    *tex_h = 1080U;
    return;
  }
  *tex_w = AlignUpDim(even_w, 64U);
  *tex_h = AlignUpDim(even_h, 16U);
}

bool ComputeCursorPatchRect(const CURSORINFO& cursor, int disp_left,
                            int disp_top, int max_w, int max_h, int* out_x0,
                            int* out_y0, int* out_x1, int* out_y1) {
  if (!(cursor.flags & CURSOR_SHOWING) || !cursor.hCursor) return false;
  struct IconGuard : ICONINFO {
    IconGuard() : ICONINFO{} {}
    ~IconGuard() {
      if (hbmColor) DeleteObject(hbmColor);
      if (hbmMask) DeleteObject(hbmMask);
    }
  } icon;
  if (!GetIconInfo(cursor.hCursor, &icon)) return false;
  BITMAP bm{};
  if (!GetObject(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(bm), &bm)) {
    return false;
  }
  const int cw = bm.bmWidth;
  const int ch = icon.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
  if (cw <= 0 || ch <= 0) return false;
  const int64_t x0 =
      static_cast<int64_t>(cursor.ptScreenPos.x) - disp_left - icon.xHotspot;
  const int64_t y0 =
      static_cast<int64_t>(cursor.ptScreenPos.y) - disp_top - icon.yHotspot;
  const int64_t x1 = x0 + cw;
  const int64_t y1 = y0 + ch;
  if (x0 >= max_w || y0 >= max_h || x1 <= 0 || y1 <= 0) return false;
  *out_x0 = static_cast<int>((std::max<int64_t>)(0, x0));
  *out_y0 = static_cast<int>((std::max<int64_t>)(0, y0));
  *out_x1 = static_cast<int>((std::min<int64_t>)(max_w, x1));
  *out_y1 = static_cast<int>((std::min<int64_t>)(max_h, y1));
  return *out_x1 > *out_x0 && *out_y1 > *out_y0;
}
}  // namespace

class GpuNv12Pool final : public std::enable_shared_from_this<GpuNv12Pool> {
 public:
  static constexpr size_t kMaxFrames = 8;

  GpuNv12Pool(Microsoft::WRL::ComPtr<ID3D11Device> device,
              Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
      : device_(std::move(device)), context_(std::move(context)) {
    if (device_) {
      device_.As(&multithread_);
    }
  }

  ~GpuNv12Pool() {
    Retire();
  }

  ID3D11Device* Device() const { return device_.Get(); }
  ID3D11DeviceContext* Context() const { return context_.Get(); }

  GpuNv12Frame* Acquire(ID3D11VideoDevice* vdev,
                        ID3D11VideoProcessorEnumerator* vp_enum,
                        uint32_t vis_w, uint32_t vis_h, uint32_t tex_w,
                        uint32_t tex_h);
  void RecycleOrDestroy(GpuNv12Frame* frame);
  void Retire();
  int CopyTextureToNv12(ID3D11Texture2D* tex, uint32_t vis_w, uint32_t vis_h,
                        uint32_t tex_w, uint32_t tex_h, uint8_t* destination,
                        size_t destination_size);

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread_;
  std::mutex mutex_;
  std::atomic<bool> retired_{false};
  std::vector<GpuNv12Frame*> free_frames_;
  size_t total_allocated_ = 0;

  std::mutex copy_mutex_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_nv12_;
  uint32_t staging_w_ = 0;
  uint32_t staging_h_ = 0;
};

class GpuNv12Frame final {
 public:
  GpuNv12Frame(std::shared_ptr<GpuNv12Pool> pool,
               Microsoft::WRL::ComPtr<ID3D11Texture2D> tex,
               Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view,
               uint32_t vis_w, uint32_t vis_h, uint32_t tex_w, uint32_t tex_h)
      : pool_(std::move(pool)),
        texture_(std::move(tex)),
        out_view_(std::move(view)),
        vis_w_(vis_w),
        vis_h_(vis_h),
        tex_w_(tex_w),
        tex_h_(tex_h) {
    descriptor_.struct_size = sizeof(descriptor_);
    descriptor_.type = MiniRtcNativeVideoFrameD3d11Nv12;
    descriptor_.width = vis_w_;
    descriptor_.height = vis_h_;
    descriptor_.payload.d3d11_nv12.d3d11_device = pool_->Device();
    descriptor_.payload.d3d11_nv12.texture = texture_.Get();
    descriptor_.payload.d3d11_nv12.subresource_index = 0;
    descriptor_.payload.d3d11_nv12.texture_width = tex_w_;
    descriptor_.payload.d3d11_nv12.texture_height = tex_h_;
    descriptor_.owner = this;
    descriptor_.retain = &RetainOwner;
    descriptor_.release = &ReleaseOwner;
    descriptor_.copy_to_nv12 = &CopyToNv12;
  }

  const MiniRtcNativeVideoFrame* Descriptor() const { return &descriptor_; }
  ID3D11VideoProcessorOutputView* OutputView() const { return out_view_.Get(); }
  size_t LogicalSize() const {
    return static_cast<size_t>(vis_w_) * vis_h_ * 3U / 2U;
  }

  void ResetRef() { references_.store(1, std::memory_order_relaxed); }

  void AddRef() { references_.fetch_add(1, std::memory_order_relaxed); }

  void Release() {
    if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (pool_) {
        pool_->RecycleOrDestroy(this);
      } else {
        delete this;
      }
    }
  }

 private:
  static void RetainOwner(void* owner) {
    if (owner) static_cast<GpuNv12Frame*>(owner)->AddRef();
  }

  static void ReleaseOwner(void* owner) {
    if (owner) static_cast<GpuNv12Frame*>(owner)->Release();
  }

  static int CopyToNv12(void* owner, uint8_t* destination,
                        size_t destination_size) {
    if (!owner) return -1;
    auto* self = static_cast<GpuNv12Frame*>(owner);
    return self->pool_
               ? self->pool_->CopyTextureToNv12(
                     self->texture_.Get(), self->vis_w_, self->vis_h_,
                     self->tex_w_, self->tex_h_, destination, destination_size)
               : -1;
  }

  std::atomic<uint32_t> references_{1};
  std::shared_ptr<GpuNv12Pool> pool_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> out_view_;
  uint32_t vis_w_ = 0;
  uint32_t vis_h_ = 0;
  uint32_t tex_w_ = 0;
  uint32_t tex_h_ = 0;
  MiniRtcNativeVideoFrame descriptor_{};
};

GpuNv12Frame* GpuNv12Pool::Acquire(ID3D11VideoDevice* vdev,
                                   ID3D11VideoProcessorEnumerator* vp_enum,
                                   uint32_t vis_w, uint32_t vis_h,
                                   uint32_t tex_w, uint32_t tex_h) {
  if (!vdev || !vp_enum || retired_.load(std::memory_order_acquire)) {
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!free_frames_.empty()) {
      GpuNv12Frame* frame = free_frames_.back();
      free_frames_.pop_back();
      frame->ResetRef();
      return frame;
    }
    if (total_allocated_ >= kMaxFrames) {
      return nullptr;
    }
    ++total_allocated_;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = tex_w;
  desc.Height = tex_h;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  HRESULT hr = device_->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
  if (FAILED(hr) || !tex) {
    std::lock_guard<std::mutex> lock(mutex_);
    --total_allocated_;
    return nullptr;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vdesc{};
  vdesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  vdesc.Texture2D.MipSlice = 0;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
  hr = vdev->CreateVideoProcessorOutputView(tex.Get(), vp_enum, &vdesc,
                                            view.GetAddressOf());
  if (FAILED(hr) || !view) {
    std::lock_guard<std::mutex> lock(mutex_);
    --total_allocated_;
    return nullptr;
  }

  return new GpuNv12Frame(shared_from_this(), std::move(tex), std::move(view),
                          vis_w, vis_h, tex_w, tex_h);
}

void GpuNv12Pool::RecycleOrDestroy(GpuNv12Frame* frame) {
  if (!frame) return;
  if (retired_.load(std::memory_order_acquire)) {
    delete frame;
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (retired_.load(std::memory_order_acquire)) {
    delete frame;
    return;
  }
  free_frames_.push_back(frame);
}

void GpuNv12Pool::Retire() {
  retired_.store(true, std::memory_order_release);
  std::vector<GpuNv12Frame*> to_delete;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    to_delete.swap(free_frames_);
  }
  for (GpuNv12Frame* frame : to_delete) {
    delete frame;
  }
}

int GpuNv12Pool::CopyTextureToNv12(ID3D11Texture2D* tex, uint32_t vis_w,
                                   uint32_t vis_h, uint32_t tex_w,
                                   uint32_t tex_h, uint8_t* destination,
                                   size_t destination_size) {
  const size_t required = static_cast<size_t>(vis_w) * vis_h * 3U / 2U;
  if (!tex || !destination || destination_size < required || !device_ ||
      !context_) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(copy_mutex_);
  if (multithread_) multithread_->Enter();
  struct MultithreadGuard {
    ID3D10Multithread* mt;
    ~MultithreadGuard() {
      if (mt) mt->Leave();
    }
  } mt_guard{multithread_.Get()};

  if (!staging_nv12_ || staging_w_ != tex_w || staging_h_ != tex_h) {
    staging_nv12_.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = tex_w;
    desc.Height = tex_h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr,
                                        staging_nv12_.GetAddressOf())) ||
        !staging_nv12_) {
      return -1;
    }
    staging_w_ = tex_w;
    staging_h_ = tex_h;
  }

  context_->CopyResource(staging_nv12_.Get(), tex);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(staging_nv12_.Get(), 0, D3D11_MAP_READ, 0,
                           &mapped))) {
    return -1;
  }
  const auto* src = static_cast<const uint8_t*>(mapped.pData);
  const size_t pitch = static_cast<size_t>(mapped.RowPitch);
  int result = 0;
  if (!src || pitch < vis_w) {
    result = -1;
  } else {
    for (uint32_t y = 0; y < vis_h; ++y) {
      std::memcpy(destination + static_cast<size_t>(y) * vis_w,
                  src + static_cast<size_t>(y) * pitch, vis_w);
    }
    const uint8_t* src_uv = src + static_cast<size_t>(tex_h) * pitch;
    uint8_t* dst_uv = destination + static_cast<size_t>(vis_w) * vis_h;
    for (uint32_t y = 0; y < vis_h / 2U; ++y) {
      std::memcpy(dst_uv + static_cast<size_t>(y) * vis_w,
                  src_uv + static_cast<size_t>(y) * pitch, vis_w);
    }
  }
  context_->Unmap(staging_nv12_.Get(), 0);
  return result;
}

class DxgiGpuNv12Converter final {
 public:
  static constexpr int kMaxCursorPatch = 256;

  DxgiGpuNv12Converter(Microsoft::WRL::ComPtr<ID3D11Device> device,
                       Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
      : device_(std::move(device)), context_(std::move(context)) {}

  ~DxgiGpuNv12Converter() {
    ResetCursorDib();
    if (pool_) pool_->Retire();
  }

  bool HasCleanBgra() const { return clean_bgra_ != nullptr && clean_bgra_valid_; }

  void ClearAcquiredViews() { acquired_input_views_.clear(); }

  bool CaptureAndConvert(ID3D11Texture2D* acquired_tex, int frame_monitor,
                         const std::vector<DisplayInfo>& displays,
                         bool show_cursor, bool* cursor_embedded,
                         bool* software_cursor_drawn,
                         GpuNv12Frame** out_frame, uint32_t* out_w,
                         uint32_t* out_h) {
    if (!acquired_tex || !out_frame || !out_w || !out_h) return false;
    *out_frame = nullptr;
    D3D11_TEXTURE2D_DESC src_desc{};
    acquired_tex->GetDesc(&src_desc);
    const uint32_t even_w = src_desc.Width & ~1U;
    const uint32_t even_h = src_desc.Height & ~1U;
    if (even_w == 0 || even_h == 0) return false;
    if (!EnsurePipeline(src_desc.Width, src_desc.Height, even_w, even_h)) {
      return false;
    }
    *out_w = even_w;
    *out_h = even_h;

    if (!show_cursor) {
      clean_bgra_valid_ = false;
      composited_has_clean_base_ = false;
      has_prev_cursor_box_ = false;
      bool need_software_draw = false;
      CURSORINFO cursor{};
      int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
      ComputeSoftwareCursorPlan(frame_monitor, displays, false, cursor_embedded,
                                &need_software_draw, &cursor, &cx0, &cy0, &cx1,
                                &cy1);
      if (software_cursor_drawn) *software_cursor_drawn = false;
      ID3D11VideoProcessorInputView* input_view =
          GetOrCreateInputView(acquired_tex);
      if (!input_view) {
        context_->CopyResource(clean_bgra_.Get(), acquired_tex);
        input_view = clean_input_view_.Get();
      }
      return BlitInputToNv12(input_view, out_frame);
    }

    context_->CopyResource(clean_bgra_.Get(), acquired_tex);
    clean_bgra_valid_ = true;
    composited_has_clean_base_ = false;
    has_prev_cursor_box_ = false;
    return ConvertFromClean(frame_monitor, displays, show_cursor,
                            /*prev_software_cursor_drawn=*/true, cursor_embedded,
                            software_cursor_drawn, out_frame);
  }

  bool ConvertFromClean(int frame_monitor,
                        const std::vector<DisplayInfo>& displays,
                        bool show_cursor, bool prev_software_cursor_drawn,
                        bool* cursor_embedded, bool* software_cursor_drawn,
                        GpuNv12Frame** out_frame) {
    if (!clean_bgra_ || !clean_bgra_valid_ || !vp_ || !video_context_ ||
        !pool_ || !out_frame) {
      return false;
    }
    *out_frame = nullptr;
    bool need_software_draw = false;
    CURSORINFO cursor{};
    int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    if (!ComputeSoftwareCursorPlan(frame_monitor, displays, show_cursor,
                                   cursor_embedded, &need_software_draw, &cursor,
                                   &cx0, &cy0, &cx1, &cy1)) {
      return false;
    }
    if (software_cursor_drawn) *software_cursor_drawn = need_software_draw;

    if (!need_software_draw && !prev_software_cursor_drawn) {
      // Nothing changed visually since the cached GPU frame was converted.
      return true;
    }

    ID3D11VideoProcessorInputView* input_view = clean_input_view_.Get();
    if (need_software_draw) {
      if (!CompositeCursorPatch(cursor, displays[frame_monitor].left,
                                displays[frame_monitor].top, cx0, cy0, cx1,
                                cy1)) {
        return false;
      }
      input_view = composited_input_view_.Get();
    }
    return BlitInputToNv12(input_view, out_frame);
  }

 private:
  bool ComputeSoftwareCursorPlan(int frame_monitor,
                                 const std::vector<DisplayInfo>& displays,
                                 bool show_cursor, bool* cursor_embedded,
                                 bool* need_software_draw, CURSORINFO* cursor,
                                 int* cx0, int* cy0, int* cx1, int* cy1) const {
    *cursor_embedded = false;
    *need_software_draw = false;
    if (frame_monitor < 0 ||
        frame_monitor >= static_cast<int>(displays.size())) {
      return true;
    }
    const auto& display = displays[frame_monitor];
    cursor->cbSize = sizeof(*cursor);
    if (!GetCursorInfo(cursor)) return true;
    const bool visible = (cursor->flags & CURSOR_SHOWING) != 0;
    const bool should_draw =
        SharedDxgiCursorState().ShouldDrawCursor(visible, display.handle);
    *cursor_embedded =
        visible && !should_draw &&
        MonitorFromPoint(cursor->ptScreenPos, MONITOR_DEFAULTTONULL) ==
            display.handle;
    if (show_cursor && should_draw) {
      if (ComputeCursorPatchRect(*cursor, display.left, display.top,
                                 static_cast<int>(vis_w_),
                                 static_cast<int>(vis_h_), cx0, cy0, cx1,
                                 cy1)) {
        if (*cx1 - *cx0 > kMaxCursorPatch || *cy1 - *cy0 > kMaxCursorPatch) {
          return false;
        }
        *need_software_draw = true;
        *cursor_embedded = true;
      }
    }
    return true;
  }

  ID3D11VideoProcessorInputView* GetOrCreateInputView(ID3D11Texture2D* tex) {
    if (!tex || !video_device_ || !vp_enum_) return nullptr;
    for (const auto& entry : acquired_input_views_) {
      if (entry.tex == tex) return entry.view.Get();
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv_desc{};
    iv_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            tex, vp_enum_.Get(), &iv_desc, view.GetAddressOf())) ||
        !view) {
      return nullptr;
    }
    if (acquired_input_views_.size() >= 8) {
      acquired_input_views_.erase(acquired_input_views_.begin());
    }
    acquired_input_views_.push_back({tex, view});
    return view.Get();
  }

  bool BlitInputToNv12(ID3D11VideoProcessorInputView* input_view,
                       GpuNv12Frame** out_frame) {
    if (!input_view || !vp_ || !video_context_ || !pool_) return false;
    GpuNv12Frame* frame = pool_->Acquire(video_device_.Get(), vp_enum_.Get(),
                                         vis_w_, vis_h_, tex_w_, tex_h_);
    if (!frame) return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view;
    const HRESULT hr = video_context_->VideoProcessorBlt(
        vp_.Get(), frame->OutputView(), frame_index_++, 1, &stream);
    if (FAILED(hr)) {
      frame->Release();
      return false;
    }
    *out_frame = frame;
    return true;
  }

 private:
  bool EnsurePipeline(uint32_t src_w, uint32_t src_h, uint32_t even_w,
                      uint32_t even_h) {
    uint32_t target_tex_w = 0, target_tex_h = 0;
    ComputeAlignedNv12TextureSize(even_w, even_h, &target_tex_w, &target_tex_h);
    if (clean_bgra_ && src_w_ == src_w && src_h_ == src_h && vis_w_ == even_w &&
        vis_h_ == even_h && tex_w_ == target_tex_w &&
        tex_h_ == target_tex_h && vp_ && pool_) {
      return true;
    }
    ResetPipeline();
    if (!device_ || !context_) return false;
    if (FAILED(device_.As(&video_device_)) || !video_device_ ||
        FAILED(context_.As(&video_context_)) || !video_context_) {
      return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cdesc{};
    cdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cdesc.InputFrameRate.Numerator = 60;
    cdesc.InputFrameRate.Denominator = 1;
    cdesc.InputWidth = even_w;
    cdesc.InputHeight = even_h;
    cdesc.OutputFrameRate.Numerator = 60;
    cdesc.OutputFrameRate.Denominator = 1;
    cdesc.OutputWidth = target_tex_w;
    cdesc.OutputHeight = target_tex_h;
    cdesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(
            &cdesc, vp_enum_.GetAddressOf())) ||
        !vp_enum_) {
      ResetPipeline();
      return false;
    }
    if (FAILED(video_device_->CreateVideoProcessor(vp_enum_.Get(), 0,
                                                   vp_.GetAddressOf())) ||
        !vp_) {
      ResetPipeline();
      return false;
    }

    D3D11_TEXTURE2D_DESC bgra_desc{};
    bgra_desc.Width = src_w;
    bgra_desc.Height = src_h;
    bgra_desc.MipLevels = 1;
    bgra_desc.ArraySize = 1;
    bgra_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bgra_desc.SampleDesc.Count = 1;
    bgra_desc.Usage = D3D11_USAGE_DEFAULT;
    bgra_desc.BindFlags = 0;
    if (FAILED(device_->CreateTexture2D(&bgra_desc, nullptr,
                                        clean_bgra_.GetAddressOf())) ||
        !clean_bgra_) {
      ResetPipeline();
      return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv_desc{};
    iv_desc.FourCC = 0;
    iv_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    iv_desc.Texture2D.MipSlice = 0;
    iv_desc.Texture2D.ArraySlice = 0;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            clean_bgra_.Get(), vp_enum_.Get(), &iv_desc,
            clean_input_view_.GetAddressOf())) ||
        !clean_input_view_) {
      ResetPipeline();
      return false;
    }

    RECT src_rect{0, 0, static_cast<LONG>(even_w), static_cast<LONG>(even_h)};
    RECT target_rect{0, 0, static_cast<LONG>(target_tex_w),
                     static_cast<LONG>(target_tex_h)};
    video_context_->VideoProcessorSetStreamFrameFormat(
        vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0,
                                                              FALSE);
    video_context_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE,
                                                      &src_rect);
    video_context_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE,
                                                    &src_rect);
    video_context_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE,
                                                      &target_rect);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.Usage = 0;
    in_cs.RGB_Range = 0;
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    video_context_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in_cs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.Usage = 0;
    out_cs.YCbCr_Matrix = 0;
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    video_context_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out_cs);

    D3D11_VIDEO_COLOR bg{};
    bg.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
    video_context_->VideoProcessorSetOutputBackgroundColor(vp_.Get(), TRUE,
                                                           &bg);

    src_w_ = src_w;
    src_h_ = src_h;
    vis_w_ = even_w;
    vis_h_ = even_h;
    tex_w_ = target_tex_w;
    tex_h_ = target_tex_h;
    frame_index_ = 0;
    pool_ = std::make_shared<GpuNv12Pool>(device_, context_);
    return true;
  }

  bool CompositeCursorPatch(const CURSORINFO& cursor, int disp_left,
                            int disp_top, int cx0, int cy0, int cx1, int cy1) {
    if (!EnsureCursorResources()) return false;
    if (!composited_has_clean_base_) {
      context_->CopyResource(composited_bgra_.Get(), clean_bgra_.Get());
      composited_has_clean_base_ = true;
      has_prev_cursor_box_ = false;
    } else if (has_prev_cursor_box_) {
      D3D11_BOX prev_box{static_cast<UINT>(prev_cx0_), static_cast<UINT>(prev_cy0_),
                         0, static_cast<UINT>(prev_cx1_),
                         static_cast<UINT>(prev_cy1_), 1};
      context_->CopySubresourceRegion(
          composited_bgra_.Get(), 0, static_cast<UINT>(prev_cx0_),
          static_cast<UINT>(prev_cy0_), 0, clean_bgra_.Get(), 0, &prev_box);
      has_prev_cursor_box_ = false;
    }
    D3D11_BOX src_box{static_cast<UINT>(cx0), static_cast<UINT>(cy0), 0,
                      static_cast<UINT>(cx1), static_cast<UINT>(cy1), 1};
    context_->CopySubresourceRegion(cursor_staging_.Get(), 0, 0, 0, 0,
                                    clean_bgra_.Get(), 0, &src_box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(cursor_staging_.Get(), 0, D3D11_MAP_READ, 0,
                             &mapped))) {
      composited_has_clean_base_ = false;
      has_prev_cursor_box_ = false;
      return false;
    }
    const int box_w = cx1 - cx0;
    const int box_h = cy1 - cy0;
    const size_t row_bytes = static_cast<size_t>(box_w) * 4U;
    const size_t dib_stride = static_cast<size_t>(kMaxCursorPatch) * 4U;
    const auto* src_bytes = static_cast<const uint8_t*>(mapped.pData);
    auto* dst_bytes = static_cast<uint8_t*>(cursor_bits_);
    for (int y = 0; y < box_h; ++y) {
      std::memcpy(dst_bytes + static_cast<size_t>(y) * dib_stride,
                  src_bytes + static_cast<size_t>(y) * mapped.RowPitch,
                  row_bytes);
    }
    context_->Unmap(cursor_staging_.Get(), 0);
    if (!DrawCursorInCapture(cursor_dc_, cursor, disp_left + cx0,
                             disp_top + cy0, box_w, box_h)) {
      return false;
    }
    GdiFlush();
    context_->UpdateSubresource(composited_bgra_.Get(), 0, &src_box,
                                cursor_bits_,
                                static_cast<UINT>(dib_stride), 0);
    prev_cx0_ = cx0;
    prev_cy0_ = cy0;
    prev_cx1_ = cx1;
    prev_cy1_ = cy1;
    has_prev_cursor_box_ = true;
    return true;
  }

  bool EnsureCursorResources() {
    if (composited_bgra_ && composited_input_view_ && cursor_staging_ &&
        cursor_bits_) {
      return true;
    }
    if (!composited_bgra_) {
      D3D11_TEXTURE2D_DESC desc{};
      clean_bgra_->GetDesc(&desc);
      if (FAILED(device_->CreateTexture2D(&desc, nullptr,
                                          composited_bgra_.GetAddressOf())) ||
          !composited_bgra_) {
        return false;
      }
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv_desc{};
      iv_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      if (FAILED(video_device_->CreateVideoProcessorInputView(
              composited_bgra_.Get(), vp_enum_.Get(), &iv_desc,
              composited_input_view_.GetAddressOf())) ||
          !composited_input_view_) {
        composited_bgra_.Reset();
        return false;
      }
    }
    if (!cursor_staging_) {
      D3D11_TEXTURE2D_DESC sdesc{};
      sdesc.Width = kMaxCursorPatch;
      sdesc.Height = kMaxCursorPatch;
      sdesc.MipLevels = 1;
      sdesc.ArraySize = 1;
      sdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      sdesc.SampleDesc.Count = 1;
      sdesc.Usage = D3D11_USAGE_STAGING;
      sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device_->CreateTexture2D(&sdesc, nullptr,
                                          cursor_staging_.GetAddressOf())) ||
          !cursor_staging_) {
        return false;
      }
    }
    if (!cursor_bits_) {
      cursor_dc_ = CreateCompatibleDC(nullptr);
      if (!cursor_dc_) return false;
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = kMaxCursorPatch;
      info.bmiHeader.biHeight = -kMaxCursorPatch;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      cursor_bmp_ = CreateDIBSection(cursor_dc_, &info, DIB_RGB_COLORS,
                                     &cursor_bits_, nullptr, 0);
      if (!cursor_bmp_ || !cursor_bits_) {
        ResetCursorDib();
        return false;
      }
      cursor_prev_bmp_ = SelectObject(cursor_dc_, cursor_bmp_);
      if (!cursor_prev_bmp_ || cursor_prev_bmp_ == HGDI_ERROR) {
        cursor_prev_bmp_ = nullptr;
        ResetCursorDib();
        return false;
      }
    }
    return true;
  }

  void ResetCursorDib() {
    if (cursor_dc_ && cursor_prev_bmp_) SelectObject(cursor_dc_, cursor_prev_bmp_);
    if (cursor_bmp_) DeleteObject(cursor_bmp_);
    if (cursor_dc_) DeleteDC(cursor_dc_);
    cursor_dc_ = nullptr;
    cursor_bmp_ = nullptr;
    cursor_prev_bmp_ = nullptr;
    cursor_bits_ = nullptr;
  }

  void ResetPipeline() {
    if (pool_) {
      pool_->Retire();
      pool_.reset();
    }
    acquired_input_views_.clear();
    composited_input_view_.Reset();
    composited_bgra_.Reset();
    clean_input_view_.Reset();
    clean_bgra_.Reset();
    cursor_staging_.Reset();
    vp_.Reset();
    vp_enum_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    clean_bgra_valid_ = false;
    composited_has_clean_base_ = false;
    has_prev_cursor_box_ = false;
    src_w_ = src_h_ = vis_w_ = vis_h_ = tex_w_ = tex_h_ = 0;
  }

  struct CachedInputView {
    ID3D11Texture2D* tex = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
  };
  std::vector<CachedInputView> acquired_input_views_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> clean_bgra_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> clean_input_view_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> composited_bgra_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> composited_input_view_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> cursor_staging_;
  HDC cursor_dc_ = nullptr;
  HBITMAP cursor_bmp_ = nullptr;
  HGDIOBJ cursor_prev_bmp_ = nullptr;
  void* cursor_bits_ = nullptr;
  std::shared_ptr<GpuNv12Pool> pool_;
  bool clean_bgra_valid_ = false;
  bool composited_has_clean_base_ = false;
  bool has_prev_cursor_box_ = false;
  int prev_cx0_ = 0;
  int prev_cy0_ = 0;
  int prev_cx1_ = 0;
  int prev_cy1_ = 0;
  uint32_t src_w_ = 0;
  uint32_t src_h_ = 0;
  uint32_t vis_w_ = 0;
  uint32_t vis_h_ = 0;
  uint32_t tex_w_ = 0;
  uint32_t tex_h_ = 0;
  uint32_t frame_index_ = 0;
};

class GpuDeliveryQueue final {
 public:
  explicit GpuDeliveryQueue(cb_desktop_data callback)
      : callback_(callback),
        thread_([this]() { WorkerLoop(); }) {}

  ~GpuDeliveryQueue() { StopAndJoin(); }

  void Enqueue(GpuNv12Frame* frame, uint32_t width, uint32_t height,
               std::string stream_id, bool cursor_embedded) {
    if (!frame) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      frame->Release();
      return;
    }
    if (pending_.frame) {
      pending_.frame->Release();
      ++dropped_;
    }
    pending_.frame = frame;
    pending_.width = width;
    pending_.height = height;
    pending_.stream_id = std::move(stream_id);
    pending_.cursor_embedded = cursor_embedded;
    pending_.has_frame = true;
    cv_.notify_one();
  }

  void DropPendingAndWaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (pending_.frame) {
      pending_.frame->Release();
      pending_.frame = nullptr;
    }
    pending_.has_frame = false;
    cv_.wait(lock, [this]() { return !in_flight_ && !pending_.has_frame; });
  }

  uint64_t ExchangeDroppedCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t count = dropped_;
    dropped_ = 0;
    return count;
  }

  void StopAndJoin() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        if (!thread_.joinable()) return;
      } else {
        stopping_ = true;
        if (pending_.frame) {
          pending_.frame->Release();
          pending_.frame = nullptr;
        }
        pending_.has_frame = false;
        cv_.notify_all();
      }
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  struct Slot {
    GpuNv12Frame* frame = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string stream_id;
    bool cursor_embedded = false;
    bool has_frame = false;
  };

  void WorkerLoop() {
    while (true) {
      Slot job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return stopping_ || pending_.has_frame; });
        if (stopping_ && !pending_.has_frame) break;
        job = std::move(pending_);
        pending_ = Slot{};
        in_flight_ = true;
      }
      if (job.frame) {
        if (callback_) {
          CapturedCursorFrameScope cursor_scope(job.cursor_embedded);
          callback_(nullptr, static_cast<int>(job.frame->LogicalSize()),
                    static_cast<int>(job.width), static_cast<int>(job.height),
                    job.stream_id.c_str(), job.frame->Descriptor());
        }
        job.frame->Release();
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_ = false;
        cv_.notify_all();
      }
    }
  }

  cb_desktop_data callback_ = nullptr;
  std::mutex mutex_;
  std::condition_variable cv_;
  Slot pending_;
  bool in_flight_ = false;
  bool stopping_ = false;
  uint64_t dropped_ = 0;
  std::thread thread_;
};

ScreenCapturerDxgi::ScreenCapturerDxgi() {}
ScreenCapturerDxgi::~ScreenCapturerDxgi() {
  Destroy();
}

int ScreenCapturerDxgi::Init(const int fps, cb_desktop_data cb) {
  fps_ = fps;
  callback_ = cb;
  if (!callback_) {
    LOG_ERROR("DXGI: callback is null");
    return -1;
  }

  display_slots_.Reset();
  display_info_list_.clear();
  topology_refresh_requested_.store(false);
  EnumerateDisplays();
  if (display_info_list_.empty()) {
    LOG_ERROR("DXGI: no displays found");
    return -3;
  }

  monitor_index_ = 0;
  return 0;
}

int ScreenCapturerDxgi::Destroy() {
  Stop();
  ReleaseDuplication();
  delivery_.reset();
  gpu_converter_.reset();
  outputs_.clear();
  display_info_list_.clear();
  display_slots_.Reset();
  d3d_context_.Reset();
  d3d_device_.Reset();
  if (nv12_frame_) {
    delete[] nv12_frame_;
    nv12_frame_ = nullptr;
    nv12_width_ = 0;
    nv12_height_ = 0;
  }
  return 0;
}

int ScreenCapturerDxgi::Start(bool show_cursor) {
  if (running_) return 0;
  show_cursor_ = show_cursor;

  if (!CreateDuplicationForMonitor(monitor_index_)) {
    LOG_ERROR("DXGI: create duplication failed for monitor {}",
              monitor_index_.load());
    return -1;
  }

  paused_ = false;
  running_ = true;
  delivery_ = std::make_unique<GpuDeliveryQueue>(callback_);
  thread_ = std::thread([this]() { CaptureLoop(); });
  return 0;
}

int ScreenCapturerDxgi::Stop() {
  if (!running_) return 0;
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (delivery_) {
    delivery_->StopAndJoin();
    delivery_.reset();
  }
  ReleaseDuplication();
  return 0;
}

int ScreenCapturerDxgi::Pause(int monitor_index) {
  paused_ = true;
  return 0;
}

int ScreenCapturerDxgi::Resume(int monitor_index) {
  paused_ = false;
  return 0;
}

int ScreenCapturerDxgi::SwitchTo(int monitor_index) {
  std::lock_guard<std::mutex> lock(switch_mutex_);
  if (monitor_index < 0 || monitor_index >= (int)display_info_list_.size()) {
    LOG_ERROR("DXGI: invalid monitor index {}", monitor_index);
    return -1;
  }
  if (monitor_index == monitor_index_ && (!running_ || duplication_)) return 0;
  // Select before Start without allocating a second D3D device/duplication.
  if (!running_) {
    monitor_index_ = monitor_index;
    return 0;
  }
  paused_ = true;
  if (delivery_) delivery_->DropPendingAndWaitIdle();
  const int previous = monitor_index_.exchange(monitor_index);
  ReleaseDuplication();
  if (!CreateDuplicationForMonitor(monitor_index)) {
    LOG_ERROR("DXGI: create duplication failed for monitor {}", monitor_index);
    // Keep the backend on the output it was capturing so the wrapper's index
    // and ours do not diverge after a rejected switch.
    monitor_index_ = previous;
    if (!CreateDuplicationForMonitor(previous)) {
      LOG_WARN("DXGI: could not restore duplication for monitor {}", previous);
    }
    paused_ = false;
    return -2;
  }
  paused_ = false;
  LOG_INFO("DXGI: switched to monitor {}:{}", monitor_index_.load(),
           display_info_list_[monitor_index_].name);
  return 0;
}

int ScreenCapturerDxgi::ResetToInitialMonitor() {
  return SwitchTo(0);
}

bool ScreenCapturerDxgi::HasDisplayTopologyChanged() const {
  if (display_info_list_.empty() ||
      display_info_list_.size() != outputs_.size()) {
    return true;
  }
  ScopedWindowsPhysicalCoordinates physical_coordinates;
  struct Win32Monitor {
    HMONITOR handle = nullptr;
    std::string name;
    bool is_primary = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };
  struct EnumContext {
    std::vector<Win32Monitor> monitors;
    bool failed = false;
  } ctx;
  const BOOL enum_ok = EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM lparam) -> BOOL {
        auto* context = reinterpret_cast<EnumContext*>(lparam);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(monitor, &mi)) {
          context->failed = true;
          return FALSE;
        }
        context->monitors.push_back(
            {monitor, GetDisplayLabel(mi.szDevice),
             (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.rcMonitor.left,
             mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  if (!enum_ok || ctx.failed || ctx.monitors.empty()) {
    return true;
  }

  size_t active_slots = 0;
  for (size_t i = 0; i < display_info_list_.size(); ++i) {
    const auto& slot = display_info_list_[i];
    if (!slot.handle) continue;
    ++active_slots;
    if (!outputs_[i]) return true;
    const auto found = std::find_if(
        ctx.monitors.begin(), ctx.monitors.end(),
        [&](const Win32Monitor& item) {
          return item.handle == static_cast<HMONITOR>(slot.handle) &&
                 item.left == slot.left && item.top == slot.top &&
                 item.right == slot.right && item.bottom == slot.bottom &&
                 item.is_primary == slot.is_primary && item.name == slot.name;
        });
    if (found == ctx.monitors.end()) {
      return true;
    }
  }
  if (active_slots != ctx.monitors.size()) {
    return true;
  }

  const int selected = monitor_index_.load(std::memory_order_relaxed);
  if (selected < 0 || selected >= static_cast<int>(outputs_.size()) ||
      !outputs_[selected]) {
    return true;
  }
  DXGI_OUTPUT_DESC out_desc{};
  if (FAILED(outputs_[selected]->GetDesc(&out_desc)) ||
      !out_desc.AttachedToDesktop || out_desc.Rotation != rotation_) {
    return true;
  }
  return false;
}

bool ScreenCapturerDxgi::EnumerateDisplays() {
  ScopedWindowsPhysicalCoordinates physical_coordinates;
  // Factories cache adapter topology. Refresh the factory as well as outputs.
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  const HRESULT factory_result =
      CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(factory_result)) {
    LOG_ERROR("DXGI: CreateDXGIFactory1 failed, hr={}", (int)factory_result);
    return false;
  }
  struct Output {
    DisplayInfo display;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    std::string identity;
  };
  std::vector<Output> current;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  for (UINT a = 0;
       factory->EnumAdapters(a, adapter.ReleaseAndGetAddressOf()) !=
       DXGI_ERROR_NOT_FOUND; ++a) {
    if (!adapter) break;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) !=
                       DXGI_ERROR_NOT_FOUND; ++o) {
      if (!output) break;
      DXGI_OUTPUT_DESC desc{};
      if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
      MONITORINFOEX mi{};
      mi.cbSize = sizeof(mi);
      if (!GetMonitorInfo(desc.Monitor, &mi)) continue;
      const auto device_id = GetOutputDeviceId(desc.DeviceName);
      current.push_back({
          DisplayInfo(desc.Monitor, GetDisplayLabel(desc.DeviceName),
                       (mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
                       mi.rcMonitor.left, mi.rcMonitor.top,
                       mi.rcMonitor.right, mi.rcMonitor.bottom),
          output, WideToUtf8(device_id.empty() ? desc.DeviceName : device_id)});
    }
  }
  std::stable_partition(current.begin(), current.end(),
                         [](const auto& item) { return item.display.is_primary; });
  std::vector<std::string> identities;
  for (const auto& item : current) identities.push_back(item.identity);
  const auto slots = display_slots_.Update(identities);
  std::vector<DisplayInfo> displays;
  std::vector<Microsoft::WRL::ComPtr<IDXGIOutput>> outputs;
  bool changed = slots.size() != display_info_list_.size();
  for (size_t slot = 0; slot < slots.size(); ++slot) {
    if (slots[slot] < 0) {
      auto missing = display_info_list_[slot];
      missing.handle = nullptr;
      missing.left = missing.top = missing.right = missing.bottom = 0;
      missing.width = missing.height = 0;
      missing.is_primary = false;
      displays.push_back(std::move(missing));
      outputs.emplace_back();
    } else {
      const auto& item = current[slots[slot]];
      displays.push_back(item.display);
      outputs.push_back(item.output);
    }
    if (slot >= display_info_list_.size()) continue;
    const auto& before = display_info_list_[slot];
    const auto& after = displays.back();
    changed = changed || before.handle != after.handle ||
              before.left != after.left || before.top != after.top ||
              before.right != after.right || before.bottom != after.bottom ||
              before.is_primary != after.is_primary || before.name != after.name;
  }
  display_info_list_ = std::move(displays);
  outputs_ = std::move(outputs);
  if (changed) {
    for (size_t slot = 0; slot < display_info_list_.size(); ++slot) {
      const auto& display = display_info_list_[slot];
      LOG_INFO("DXGI: session display slot={} name='{}' available={} "
               "bounds=({},{};{},{}), selected={}",
               slot, display.name, outputs_[slot] != nullptr,
               display.left, display.top, display.right, display.bottom,
               static_cast<int>(slot) == monitor_index_.load());
    }
  }
  return changed;
}

bool ScreenCapturerDxgi::CreateDuplicationForMonitor(int monitor_index) {
  SharedDxgiCursorState().Reset();
  if (monitor_index < 0 || monitor_index >= (int)outputs_.size() ||
      !outputs_[monitor_index]) return false;

  // Outputs may move between GPU adapters when the display topology changes.
  // Desktop Duplication requires the D3D device to be created from the same
  // adapter as the selected IDXGIOutput.
  Microsoft::WRL::ComPtr<IDXGIAdapter> output_adapter;
  HRESULT hr = outputs_[monitor_index]->GetParent(
      IID_PPV_ARGS(output_adapter.GetAddressOf()));
  if (FAILED(hr) || !output_adapter) {
    LOG_ERROR("DXGI: get adapter for output {} failed, hr={}", monitor_index,
              (int)hr);
    return false;
  }
  DXGI_ADAPTER_DESC adapter_desc{};
  output_adapter->GetDesc(&adapter_desc);

  Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
  hr = outputs_[monitor_index]->QueryInterface(
      IID_PPV_ARGS(output1.GetAddressOf()));
  if (FAILED(hr)) {
    LOG_ERROR("DXGI: Query IDXGIOutput1 failed, hr={}", (int)hr);
    return false;
  }

  duplication_.Reset();
  bool reused_device = false;
  if (d3d_device_ && d3d_context_ &&
      SUCCEEDED(d3d_device_->GetDeviceRemovedReason())) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> current_adapter;
    DXGI_ADAPTER_DESC current_desc{};
    if (SUCCEEDED(d3d_device_.As(&dxgi_device)) && dxgi_device &&
        SUCCEEDED(dxgi_device->GetAdapter(current_adapter.GetAddressOf())) &&
        current_adapter && SUCCEEDED(current_adapter->GetDesc(&current_desc)) &&
        current_desc.AdapterLuid.LowPart == adapter_desc.AdapterLuid.LowPart &&
        current_desc.AdapterLuid.HighPart ==
            adapter_desc.AdapterLuid.HighPart) {
      hr = output1->DuplicateOutput(d3d_device_.Get(),
                                    duplication_.GetAddressOf());
      if (SUCCEEDED(hr) && duplication_) {
        reused_device = true;
      }
    }
  }

  if (!reused_device) {
    gpu_converter_.reset();
    d3d_context_.Reset();
    d3d_device_.Reset();

    UINT flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL out_level{};
    Microsoft::WRL::ComPtr<ID3D11Device> output_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> output_context;
    hr = D3D11CreateDevice(
        output_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
        feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
        output_device.GetAddressOf(), &out_level,
        output_context.GetAddressOf());
    if (FAILED(hr)) {
      UINT fallback_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
      fallback_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
      hr = D3D11CreateDevice(
          output_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
          fallback_flags, feature_levels, ARRAYSIZE(feature_levels),
          D3D11_SDK_VERSION, output_device.GetAddressOf(), &out_level,
          output_context.GetAddressOf());
    }
    if (FAILED(hr)) {
      LOG_ERROR("DXGI: create D3D device for output adapter {} failed, hr={}",
                monitor_index, (int)hr);
      return false;
    }
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(output_device.As(&multithread)) && multithread) {
      multithread->SetMultithreadProtected(TRUE);
    }
    d3d_context_ = std::move(output_context);
    d3d_device_ = std::move(output_device);

    hr = output1->DuplicateOutput(d3d_device_.Get(),
                                  duplication_.GetAddressOf());
    if (FAILED(hr)) {
      LOG_ERROR("DXGI: DuplicateOutput failed, hr={}", (int)hr);
      return false;
    }
  }

  staging_.Reset();
  DXGI_OUTDUPL_DESC desc{};
  duplication_->GetDesc(&desc);
  rotation_ = desc.Rotation;
  DXGI_OUTPUT_DESC output_desc{};
  software_cursor_output_ =
      SUCCEEDED(outputs_[monitor_index]->GetDesc(&output_desc)) &&
      IsUsbmmiddDisplayDevice(output_desc.DeviceName);
  SharedDxgiCursorState().BeginOutput(
      display_info_list_[monitor_index].handle, software_cursor_output_);
  last_logged_cursor_embedding_ = -1;
  LOG_INFO("DXGI: duplication ready, monitor={}, rotation={}, adapter='{}'",
           monitor_index, static_cast<int>(rotation_),
           WideToUtf8(adapter_desc.Description));
  return true;
}

void ScreenCapturerDxgi::ReleaseDuplication() {
  // A stopped candidate may be destroyed after its replacement has started.
  // Only the live duplication owns the shared pointer-plane state.
  if (duplication_) SharedDxgiCursorState().Reset();
  ++duplication_generation_;
  if (gpu_converter_) gpu_converter_->ClearAcquiredViews();
  staging_.Reset();
  ReleaseAcquiredFrame();
  duplication_.Reset();
}

HRESULT ScreenCapturerDxgi::ReleaseAcquiredFrame() {
  if (!frame_acquired_) return S_OK;
  frame_acquired_ = false;
  const HRESULT result = duplication_->ReleaseFrame();
  if (FAILED(result))
    LOG_WARN("DXGI: ReleaseFrame failed, hr=0x{:08X}",
             static_cast<uint32_t>(result));
  return result;
}

bool ScreenCapturerDxgi::ConvertFrame(int frame_monitor, bool* cursor_embedded,
                                      CursorFrameCompositor& compositor,
                                      bool* software_cursor_drawn) {
  *cursor_embedded = false;
  if (software_cursor_drawn) *software_cursor_drawn = false;
  if (!staging_) return false;
  D3D11_TEXTURE2D_DESC src_desc{};
  staging_->GetDesc(&src_desc);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT hr =
      d3d_context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    return false;
  }

  auto pixels = static_cast<const uint8_t*>(mapped.pData);
  int stride = static_cast<int>(mapped.RowPitch);
  int logical_width = static_cast<int>(src_desc.Width);
  int logical_height = static_cast<int>(src_desc.Height);
  libyuv::RotationMode rotation = libyuv::kRotate0;
  switch (rotation_) {
    case DXGI_MODE_ROTATION_ROTATE90:
      rotation = libyuv::kRotate90;
      break;
    case DXGI_MODE_ROTATION_ROTATE180:
      rotation = libyuv::kRotate180;
      break;
    case DXGI_MODE_ROTATION_ROTATE270:
      rotation = libyuv::kRotate270;
      break;
    default:
      break;
  }
  if (rotation != libyuv::kRotate0) {
    const bool swap_axes = rotation != libyuv::kRotate180;
    const int rotated_width = swap_axes ? logical_height : logical_width;
    rotated_frame_.resize(static_cast<size_t>(logical_width) * logical_height *
                          4);
    if (libyuv::ARGBRotate(pixels, stride, rotated_frame_.data(),
                           rotated_width * 4, logical_width, logical_height,
                           rotation) != 0) {
      d3d_context_->Unmap(staging_.Get(), 0);
      return false;
    }
    pixels = rotated_frame_.data();
    stride = rotated_width * 4;
    if (swap_axes) std::swap(logical_width, logical_height);
  }
  int even_width = logical_width & ~1;
  int even_height = logical_height & ~1;
  if (even_width <= 0 || even_height <= 0) {
    d3d_context_->Unmap(staging_.Get(), 0);
    return false;
  }

  if (frame_monitor >= 0 &&
      frame_monitor < static_cast<int>(display_info_list_.size())) {
    const auto& display = display_info_list_[frame_monitor];
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (GetCursorInfo(&cursor)) {
      const bool visible = (cursor.flags & CURSOR_SHOWING) != 0;
      const bool should_draw = SharedDxgiCursorState().ShouldDrawCursor(
          visible, display.handle);
      // show_cursor controls our extra composition, not cursors the OS has
      // already put in the texture. Publish that distinction for native
      // controllers as well, so they do not draw a second cursor.
      *cursor_embedded = visible && !should_draw &&
          MonitorFromPoint(cursor.ptScreenPos, MONITOR_DEFAULTTONULL) ==
              display.handle;
      if (show_cursor_.load(std::memory_order_relaxed) && should_draw) {
        if (const auto* composited =
                compositor.Draw(pixels, stride, even_width, even_height, cursor,
                                display.left, display.top)) {
          pixels = composited;
          stride = even_width * 4;
          *cursor_embedded = true;
          if (software_cursor_drawn) *software_cursor_drawn = true;
        }
      }
    }
  }

  int nv12_size = even_width * even_height * 3 / 2;
  if (!nv12_frame_ || nv12_width_ != even_width ||
      nv12_height_ != even_height) {
    delete[] nv12_frame_;
    nv12_frame_ = new unsigned char[nv12_size];
    nv12_width_ = even_width;
    nv12_height_ = even_height;
  }

  const int converted =
      libyuv::ARGBToNV12(pixels, stride, nv12_frame_, even_width,
                         nv12_frame_ + even_width * even_height, even_width,
                         even_width, even_height);

  d3d_context_->Unmap(staging_.Get(), 0);
  return converted == 0;
}

void ScreenCapturerDxgi::CaptureLoop() {
  ScopedWindowsPhysicalCoordinates physical_coordinates;
  struct ScopedTimerPeriod {
    bool active = false;
    ScopedTimerPeriod() { active = (timeBeginPeriod(1) == TIMERR_NOERROR); }
    ~ScopedTimerPeriod() {
      if (active) timeEndPeriod(1);
    }
  } timer_period;
  CursorFrameCompositor cursor_compositor;
  constexpr auto kFrameDeadlineTolerance = std::chrono::microseconds(3500);
  const auto frame_interval =
      std::chrono::nanoseconds(1'000'000'000) / (std::max)(1, fps_);
  auto next_frame_deadline = std::chrono::steady_clock::now();
  bool cached_frame_valid = false;
  bool cached_frame_fresh = false;
  bool cached_cursor_embedded = false;
  bool cached_software_cursor_drawn = false;
  int cached_monitor = -1;
  uint64_t cached_generation = 0;
  auto next_topology_check = std::chrono::steady_clock::now();
  unsigned acquire_failures = 0;
  auto cursor_revision = SharedDxgiCursorState().CursorVisibilityRevision();
  struct GpuFrameHolder {
    GpuNv12Frame* frame = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    ~GpuFrameHolder() { Reset(); }
    void Reset() {
      if (frame) {
        frame->Release();
        frame = nullptr;
      }
      width = height = 0;
    }
    void Set(GpuNv12Frame* next, uint32_t w, uint32_t h) {
      if (frame) frame->Release();
      frame = next;
      width = w;
      height = h;
    }
  } cached_gpu_frame;
  auto clear_cached = [&] {
    cached_frame_valid = false;
    cached_frame_fresh = false;
    cached_software_cursor_drawn = false;
    cached_gpu_frame.Reset();
  };
  auto stat_window_start = std::chrono::steady_clock::now();
  uint64_t stat_acquired = 0;
  uint64_t stat_cached_replays = 0;
  uint64_t stat_delivered = 0;
  uint64_t stat_access_lost = 0;
  std::vector<double> stat_conv_ms;
  stat_conv_ms.reserve(512);
  auto maybe_log_capture_stats = [&](std::chrono::steady_clock::time_point now_tp) {
    const double elapsed_s =
        std::chrono::duration<double>(now_tp - stat_window_start).count();
    if (elapsed_s < 5.0) return;
    double p50 = 0.0, p95 = 0.0, max_ms = 0.0;
    if (!stat_conv_ms.empty()) {
      const size_t n = stat_conv_ms.size();
      const size_t idx50 = std::min(n - 1, (n - 1) / 2);
      const size_t idx95 =
          std::min(n - 1, static_cast<size_t>(0.95 * static_cast<double>(n - 1)));
      std::nth_element(stat_conv_ms.begin(), stat_conv_ms.begin() + idx50,
                       stat_conv_ms.end());
      p50 = stat_conv_ms[idx50];
      std::nth_element(stat_conv_ms.begin(), stat_conv_ms.begin() + idx95,
                       stat_conv_ms.end());
      p95 = stat_conv_ms[idx95];
      max_ms = *std::max_element(stat_conv_ms.begin(), stat_conv_ms.end());
    }
    const uint64_t deliv_drop =
        delivery_ ? delivery_->ExchangeDroppedCount() : 0;
    LOG_INFO(
        "DXGI capture stats ({:.1f}s): delivered={} fps={:.1f} acquired={} "
        "cached={} deliv_drop={} access_lost={} conv_ms[p50={:.2f} p95={:.2f} "
        "max={:.2f}]",
        elapsed_s, stat_delivered,
        static_cast<double>(stat_delivered) / elapsed_s, stat_acquired,
        stat_cached_replays, deliv_drop, stat_access_lost, p50, p95, max_ms);
    stat_window_start = now_tp;
    stat_acquired = 0;
    stat_cached_replays = 0;
    stat_delivered = 0;
    stat_access_lost = 0;
    stat_conv_ms.clear();
  };
  auto advance_frame_deadline =
      [&](std::chrono::steady_clock::time_point ref_now) {
        next_frame_deadline =
            (std::max)(next_frame_deadline + frame_interval,
                       ref_now + frame_interval / 2);
      };
  while (running_) {
    if (paused_) {
      clear_cached();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Duplication/staging textures belong to one monitor. Do not release or
    // replace them in SwitchTo while AcquireNextFrame/Map is using them.
    std::unique_lock capture_lock(switch_mutex_);
    if (paused_ || !running_) continue;
    const auto now = std::chrono::steady_clock::now();
    const auto revision = SharedDxgiCursorState().CursorVisibilityRevision();
    // An IDD can bake the pointer into the last desktop texture. Retire that
    // cached texture when the user helper hides/restores the system cursor;
    // pointer-only updates must not keep replaying its old image.
    const bool refresh = topology_refresh_requested_.exchange(false) ||
                         revision != cursor_revision;
    cursor_revision = revision;
    if (refresh || now >= next_topology_check) {
      next_topology_check = now + std::chrono::milliseconds(500);
      const bool should_enumerate =
          refresh || !duplication_ || HasDisplayTopologyChanged();
      const bool changed = should_enumerate ? EnumerateDisplays() : false;
      if (changed || refresh || !duplication_) {
        const int target_monitor = monitor_index_.load();
        const ID3D11Device* prev_device = d3d_device_.Get();
        if (changed || refresh || cached_monitor != target_monitor) {
          clear_cached();
        }
        ReleaseDuplication();
        if (!CreateDuplicationForMonitor(target_monitor) ||
            d3d_device_.Get() != prev_device) {
          clear_cached();
        } else if (cached_frame_valid) {
          cached_generation = duplication_generation_;
        }
        capture_lock.unlock();
        if ((changed || refresh) && callback_) {
          if (delivery_) delivery_->DropPendingAndWaitIdle();
          callback_(nullptr, ScreenCapturer::kDisplayTopologyChanged,
                    0, 0, "", nullptr);
        }
        continue;
      }
    }
    if (!duplication_) {
      clear_cached();
      capture_lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    const int frame_monitor = monitor_index_.load();
    const auto deliver_cached = [&] {
      if (!cached_frame_valid || cached_generation != duplication_generation_ ||
          cached_monitor != monitor_index_.load() || !callback_)
        return;
      const bool show_cursor = show_cursor_.load(std::memory_order_relaxed);
      const bool was_fresh = cached_frame_fresh;
      const bool need_cursor_recompose =
          !cached_frame_fresh &&
          (show_cursor || cached_software_cursor_drawn);
      cached_frame_fresh = false;
      if (cached_gpu_frame.frame) {
        if (need_cursor_recompose && gpu_converter_ &&
            gpu_converter_->HasCleanBgra()) {
          GpuNv12Frame* rebuilt = nullptr;
          if (gpu_converter_->ConvertFromClean(
                  cached_monitor, display_info_list_, show_cursor,
                  cached_software_cursor_drawn, &cached_cursor_embedded,
                  &cached_software_cursor_drawn, &rebuilt)) {
            if (rebuilt) {
              cached_gpu_frame.Set(rebuilt, cached_gpu_frame.width,
                                   cached_gpu_frame.height);
            }
          } else {
            cached_gpu_frame.Reset();
          }
        }
        if (cached_gpu_frame.frame) {
          GpuNv12Frame* emit_frame = cached_gpu_frame.frame;
          emit_frame->AddRef();
          const uint32_t ew = cached_gpu_frame.width;
          const uint32_t eh = cached_gpu_frame.height;
          const bool embedded = cached_cursor_embedded;
          auto stream_id = MakeDisplayStreamId(cached_monitor);
          capture_lock.unlock();
          if (!was_fresh) ++stat_cached_replays;
          ++stat_delivered;
          if (delivery_) {
            delivery_->Enqueue(emit_frame, ew, eh, std::move(stream_id),
                               embedded);
          } else {
            CapturedCursorFrameScope cursor_scope(embedded);
            callback_(nullptr, static_cast<int>(emit_frame->LogicalSize()),
                      static_cast<int>(ew), static_cast<int>(eh),
                      stream_id.c_str(), emit_frame->Descriptor());
            emit_frame->Release();
          }
          maybe_log_capture_stats(std::chrono::steady_clock::now());
          return;
        }
      }
      if (!nv12_frame_) return;
      // Recompose from the clean staging texture while showing a cursor, or
      // once after disabling it. Never replay a baked-in stale cursor.
      if (need_cursor_recompose &&
          !ConvertFrame(cached_monitor, &cached_cursor_embedded,
                        cursor_compositor, &cached_software_cursor_drawn)) {
        clear_cached();
        return;
      }
      const auto stream_id = MakeDisplayStreamId(cached_monitor);
      capture_lock.unlock();
      if (delivery_) delivery_->DropPendingAndWaitIdle();
      if (!was_fresh) ++stat_cached_replays;
      ++stat_delivered;
      CapturedCursorFrameScope cursor_scope(cached_cursor_embedded);
      callback_(nv12_frame_, nv12_width_ * nv12_height_ * 3 / 2, nv12_width_,
                nv12_height_, stream_id.c_str(), nullptr);
      maybe_log_capture_stats(std::chrono::steady_clock::now());
    };
    const auto try_fast_reduplicate = [&]() -> bool {
      if (HasDisplayTopologyChanged()) {
        ReleaseDuplication();
        next_topology_check = std::chrono::steady_clock::now();
        return false;
      }
      const int target_monitor = monitor_index_.load();
      const ID3D11Device* prev_device = d3d_device_.Get();
      const DXGI_MODE_ROTATION prev_rotation = rotation_;
      if (cached_monitor != target_monitor) {
        clear_cached();
      }
      ReleaseDuplication();
      if (!CreateDuplicationForMonitor(target_monitor) ||
          d3d_device_.Get() != prev_device || rotation_ != prev_rotation) {
        clear_cached();
        if (rotation_ != prev_rotation) {
          topology_refresh_requested_.store(true);
        }
        next_topology_check = std::chrono::steady_clock::now();
        return false;
      }
      if (cached_frame_valid) {
        cached_generation = duplication_generation_;
      }
      const auto recover_now = std::chrono::steady_clock::now();
      if (cached_frame_valid &&
          recover_now + kFrameDeadlineTolerance >= next_frame_deadline) {
        advance_frame_deadline(recover_now);
        deliver_cached();
      }
      return true;
    };
    const auto before_acquire = std::chrono::steady_clock::now();
    if (cached_frame_fresh &&
        before_acquire + kFrameDeadlineTolerance >= next_frame_deadline) {
      advance_frame_deadline(before_acquire);
      deliver_cached();
      continue;
    }
    int wait_ms = 1;
    if (next_frame_deadline > before_acquire) {
      const auto remaining = next_frame_deadline - before_acquire;
      wait_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              remaining > std::chrono::milliseconds(2)
                  ? remaining - std::chrono::milliseconds(1)
                  : remaining)
              .count());
      if (wait_ms < 1) wait_ms = 1;
    }
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktop_resource;
    HRESULT hr = duplication_->AcquireNextFrame(
        wait_ms, &frame_info, desktop_resource.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      // DXGI explicitly reports an unchanged desktop. Re-emit only a frame
      // successfully acquired for this monitor to keep delivering static frames.
      const auto timeout_now = std::chrono::steady_clock::now();
      if (timeout_now + kFrameDeadlineTolerance < next_frame_deadline) {
        continue;
      }
      advance_frame_deadline(timeout_now);
      deliver_cached();
      continue;
    }
    // Never replay a cached image after repeated acquisition failures or
    // a backend rebuild. Display recovery retains the session privacy state.
    if (FAILED(hr)) {
      LOG_ERROR("DXGI: AcquireNextFrame failed, hr={}", (int)hr);
      ++stat_access_lost;
      // A single ACCESS_LOST (mode switch, fullscreen toggle, UAC return) is
      // rebuilt immediately on the same monitor without a full topology probe.
      // Only repeated failures fall back to the 500 ms topology cadence so a
      // broken output does not spin while recreating D3D devices.
      if (++acquire_failures == 1) {
        try_fast_reduplicate();
      } else {
        ReleaseDuplication();
        clear_cached();
      }
      maybe_log_capture_stats(std::chrono::steady_clock::now());
      continue;
    }
    frame_acquired_ = true;
    acquire_failures = 0;

    if (frame_monitor >= 0 &&
        frame_monitor < static_cast<int>(display_info_list_.size())) {
      SharedDxgiCursorState().Update(
          frame_info.LastMouseUpdateTime.QuadPart,
          frame_info.PointerPosition.Visible != FALSE,
          display_info_list_[frame_monitor].handle);
    }

    // A successful acquire can contain only a cursor update. It is not a new
    // desktop image; in particular, do not seed a newly rebuilt capture with
    // its surface. Keep the last confirmed desktop until a present arrives.
    if (frame_info.LastPresentTime.QuadPart == 0) {
      if (FAILED(ReleaseAcquiredFrame())) {
        ++stat_access_lost;
        try_fast_reduplicate();
      } else {
        const auto after_pointer = std::chrono::steady_clock::now();
        if (after_pointer + kFrameDeadlineTolerance >= next_frame_deadline) {
          advance_frame_deadline(after_pointer);
          deliver_cached();
        }
      }
      continue;
    }

    const auto convert_start = std::chrono::steady_clock::now();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquired_tex;
    if (desktop_resource) {
      hr = desktop_resource->QueryInterface(
          IID_PPV_ARGS(acquired_tex.GetAddressOf()));
      if (FAILED(hr)) {
        ReleaseAcquiredFrame();
        continue;
      }
    } else {
      ReleaseAcquiredFrame();
      continue;
    }

    bool cursor_embedded = false;
    bool software_cursor_drawn = false;
    GpuNv12Frame* gpu_frame = nullptr;
    uint32_t gpu_w = 0, gpu_h = 0;
    if (rotation_ == DXGI_MODE_ROTATION_IDENTITY) {
      if (!gpu_converter_ && d3d_device_ && d3d_context_) {
        gpu_converter_ = std::make_unique<DxgiGpuNv12Converter>(d3d_device_,
                                                                d3d_context_);
      }
      if (gpu_converter_ &&
          !gpu_converter_->CaptureAndConvert(
              acquired_tex.Get(), frame_monitor, display_info_list_,
              show_cursor_.load(std::memory_order_relaxed), &cursor_embedded,
              &software_cursor_drawn, &gpu_frame, &gpu_w, &gpu_h)) {
        if (gpu_frame) {
          gpu_frame->Release();
          gpu_frame = nullptr;
        }
      }
    }

    bool converted = (gpu_frame != nullptr);
    if (!converted) {
      D3D11_TEXTURE2D_DESC src_desc{};
      acquired_tex->GetDesc(&src_desc);

      if (!staging_) {
        D3D11_TEXTURE2D_DESC staging_desc = src_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr,
                                          staging_.GetAddressOf());
        if (FAILED(hr)) {
          LOG_ERROR("DXGI: CreateTexture2D staging failed, hr={}", (int)hr);
          ReleaseAcquiredFrame();
          continue;
        }
      }

      d3d_context_->CopyResource(staging_.Get(), acquired_tex.Get());
      converted =
          ConvertFrame(frame_monitor, &cursor_embedded, cursor_compositor,
                       &software_cursor_drawn);
    }

    if (converted && frame_monitor == monitor_index_.load() && callback_) {
      if (last_logged_cursor_embedding_ < 0 ||
          (software_cursor_output_ &&
           last_logged_cursor_embedding_ != static_cast<int>(cursor_embedded))) {
        LOG_DEBUG("DXGI cursor: monitor={} usbmmidd={} mouse_update={} "
                 "separate_pointer_visible={} pointer_shape_bytes={} "
                 "compose_cursor={} embedded={}",
                 frame_monitor, software_cursor_output_,
                 frame_info.LastMouseUpdateTime.QuadPart,
                 frame_info.PointerPosition.Visible != FALSE,
                 frame_info.PointerShapeBufferSize,
                 show_cursor_.load(std::memory_order_relaxed), cursor_embedded);
        last_logged_cursor_embedding_ = static_cast<int>(cursor_embedded);
      }
    }
    // A mode/desktop transition can invalidate the acquire during readback.
    // Do not publish that frame if releasing it reports the lost capture.
    if (FAILED(ReleaseAcquiredFrame())) {
      ++stat_access_lost;
      if (gpu_frame) {
        gpu_frame->Release();
        gpu_frame = nullptr;
      }
      try_fast_reduplicate();
      continue;
    }

    if (converted && frame_monitor == monitor_index_.load() && callback_) {
      const auto after_convert = std::chrono::steady_clock::now();
      ++stat_acquired;
      stat_conv_ms.push_back(
          std::chrono::duration<double, std::milli>(after_convert - convert_start)
              .count());
      const bool first_valid_frame = !cached_frame_valid;
      if (gpu_frame) {
        cached_gpu_frame.Set(gpu_frame, gpu_w, gpu_h);
      } else {
        cached_gpu_frame.Reset();
      }
      cached_cursor_embedded = cursor_embedded;
      cached_software_cursor_drawn = software_cursor_drawn;
      cached_monitor = frame_monitor;
      cached_generation = duplication_generation_;
      cached_frame_valid = true;
      cached_frame_fresh = true;

      if (first_valid_frame) {
        next_frame_deadline = after_convert + frame_interval;
        deliver_cached();
      } else if (after_convert + kFrameDeadlineTolerance >=
                 next_frame_deadline) {
        advance_frame_deadline(after_convert);
        deliver_cached();
      } else {
        maybe_log_capture_stats(after_convert);
      }
    } else {
      if (gpu_frame) {
        gpu_frame->Release();
      }
      if (!converted) {
        clear_cached();
      }
    }
  }
}

}  // namespace crossdesk
