/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "gbmbufferhandler.h"

#include <unistd.h>
#include <drm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include <hwcdefs.h>
#include <hwctrace.h>
#include <platformdefines.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "commondrmutils.h"

namespace hwcomposer {

// static
NativeBufferHandler *NativeBufferHandler::CreateInstance(uint32_t fd) {
  GbmBufferHandler *handler = new GbmBufferHandler(fd);
  if (!handler)
    return NULL;

  if (!handler->Init()) {
    ETRACE("Failed to initialize GbmBufferHandler.");
    delete handler;
    return NULL;
  }
  return handler;
}

GbmBufferHandler::GbmBufferHandler(uint32_t fd) : fd_(fd), device_(0) {
}

GbmBufferHandler::~GbmBufferHandler() {
  if (device_)
    gbm_device_destroy(device_);
}

bool GbmBufferHandler::Init() {
  device_ = gbm_create_device(fd_);
  if (!device_) {
    ETRACE("failed to create gbm device \n");
    return false;
  }

  return true;
}

bool GbmBufferHandler::CreateBuffer(uint32_t w, uint32_t h, int format,
                                    HWCNativeHandle *handle,
                                    uint32_t layer_type) const {
  uint32_t gbm_format = format;
  if (gbm_format == 0)
    gbm_format = GBM_FORMAT_XRGB8888;

  uint32_t flags = 0;

  if (layer_type == kLayerNormal) {
    flags |= (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
#ifdef USE_MINIGBM
  } else if (layer_type == kLayerVideo) {
    flags |= (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING |
              GBM_BO_USE_CAMERA_WRITE | GBM_BO_USE_CAMERA_READ);
#endif
  }

  struct gbm_bo *bo = gbm_bo_create(device_, w, h, gbm_format, flags);

  if (!bo) {
    flags &= ~GBM_BO_USE_SCANOUT;
    bo = gbm_bo_create(device_, w, h, gbm_format, flags);
  }

  if (!bo) {
    flags &= ~GBM_BO_USE_RENDERING;
    bo = gbm_bo_create(device_, w, h, gbm_format, flags);
  }

  if (!bo) {
    ETRACE("GbmBufferHandler: failed to create gbm_bo");
    return false;
  }

  struct gbm_handle *temp = new struct gbm_handle();
  temp->import_data.width = gbm_bo_get_width(bo);
  temp->import_data.height = gbm_bo_get_height(bo);
  temp->import_data.format = gbm_bo_get_format(bo);
#if USE_MINIGBM
  size_t total_planes = gbm_bo_get_num_planes(bo);
  for (size_t i = 0; i < total_planes; i++) {
    temp->import_data.fds[i] = gbm_bo_get_plane_fd(bo, i);
    temp->import_data.offsets[i] = gbm_bo_get_plane_offset(bo, i);
    temp->import_data.strides[i] = gbm_bo_get_plane_stride(bo, i);
  }
  temp->total_planes = total_planes;
#else
  temp->import_data.fd = gbm_bo_get_fd(bo);
  temp->import_data.stride = gbm_bo_get_stride(bo);
  temp->total_planes = drm_bo_get_num_planes(temp->import_data.format);
#endif

  temp->bo = bo;
  temp->hwc_buffer_ = true;
  temp->gbm_flags = flags;
  *handle = temp;

  return true;
}

bool GbmBufferHandler::ReleaseBuffer(HWCNativeHandle handle) const {
  if (handle->bo || handle->imported_bo) {
    if (handle->bo && handle->hwc_buffer_) {
      gbm_bo_destroy(handle->bo);
    }

    if (handle->imported_bo) {
      gbm_bo_destroy(handle->imported_bo);
    }
#ifdef USE_MINIGBM
    for (size_t i = 0; i < handle->total_planes; i++)
      close(handle->import_data.fds[i]);
#else
    close(handle->import_data.fd);
#endif
  }

  return true;
}

void GbmBufferHandler::DestroyHandle(HWCNativeHandle handle) const {
  delete handle;
  handle = NULL;
}

void GbmBufferHandler::CopyHandle(HWCNativeHandle source,
                                  HWCNativeHandle *target) const {
  struct gbm_handle *temp = new struct gbm_handle();
  temp->import_data.width = source->import_data.width;
  temp->import_data.height = source->import_data.height;
  temp->import_data.format = source->import_data.format;
#if USE_MINIGBM
  size_t total_planes = source->total_planes;
  for (size_t i = 0; i < total_planes; i++) {
    temp->import_data.fds[i] = dup(source->import_data.fds[i]);
    temp->import_data.offsets[i] = source->import_data.offsets[i];
    temp->import_data.strides[i] = source->import_data.strides[i];
  }
#else
  temp->import_data.fd = dup(source->import_data.fd);
  temp->import_data.stride = source->import_data.stride;
#endif
  temp->bo = source->bo;
  temp->total_planes = source->total_planes;
  temp->gbm_flags  = source->gbm_flags;
  temp->is_dumb_buffer = source->is_dumb_buffer;
  temp->dumb_buffer_mem = source->dumb_buffer_mem;

  *target = temp;
}

bool GbmBufferHandler::ImportBuffer(HWCNativeHandle handle) const {
  memset(&(handle->meta_data_), 0, sizeof(struct HwcBuffer));
  uint32_t gem_handle = 0;
  int ret;
  handle->meta_data_.format_ = handle->import_data.format;
  handle->meta_data_.native_format_ = handle->import_data.format;
  if (handle->is_dumb_buffer) {
    struct drm_mode_create_dumb request;
    struct drm_mode_map_dumb map_arg;
    void* map;
    memset(&request, 0, sizeof(request));
    request.width = handle->import_data.width;
    request.height = handle->import_data.height;
    request.bpp = 4; //handle->import_data.stride / handle->import_data.width;
    request.flags = 0;
    if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &request) < 0) {
      ETRACE("Failed to create dumb buffer %d %s", errno, strerror(errno));
    }

    gem_handle = request.handle;

    memset(&map_arg, 0, sizeof map_arg);
    map_arg.handle = gem_handle;
    ret = drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);
    if(ret) {
      ETRACE("hkps failed ioctl %d %s", errno, strerror(errno));
    }
    ETRACE("hkps %dx%d %d", handle->import_data.width, handle->import_data.height, gem_handle);
    map = mmap(NULL, handle->import_data.width * handle->import_data.height, PROT_WRITE,
               MAP_SHARED, fd_, map_arg.offset);
    if (map == MAP_FAILED) {
      ETRACE("unable to map buffer %d %s", errno, strerror(errno));
      raise(SIGSEGV);
    }

    memcpy(map, handle->dumb_buffer_mem, handle->import_data.height*handle->import_data.stride);

  } else if (!handle->imported_bo) {
#if USE_MINIGBM
    handle->imported_bo =
        gbm_bo_import(device_, GBM_BO_IMPORT_FD_PLANAR, &handle->import_data,
                      handle->gbm_flags);
     if (!handle->imported_bo) {
        ETRACE("can't import bo");
      }
#else
    handle->imported_bo = gbm_bo_import(
        device_, GBM_BO_IMPORT_FD, &handle->import_data, handle->gbm_flags);
    if (!handle->imported_bo) {
        ETRACE("can't import bo");
    }
#endif
    gem_handle = gbm_bo_get_handle(handle->imported_bo).u32;
  }


  if (!gem_handle) {
    ETRACE("Invalid GEM handle. \n");
    return false;
  }

  handle->meta_data_.width_ = handle->import_data.width;
  handle->meta_data_.height_ = handle->import_data.height;
  // FIXME: Set right flag here.
  handle->meta_data_.usage_ = hwcomposer::kLayerNormal;

#if USE_MINIGBM
  handle->meta_data_.prime_fd_ = handle->import_data.fds[0];
  size_t total_planes = gbm_bo_get_num_planes(handle->bo);
  for (size_t i = 0; i < total_planes; i++) {
    handle->meta_data_.gem_handles_[i] = gem_handle;
    handle->meta_data_.offsets_[i] = gbm_bo_get_plane_offset(handle->bo, i);
    handle->meta_data_.pitches_[i] = gbm_bo_get_plane_stride(handle->bo, i);
  }
#else
  handle->meta_data_.prime_fd_ = handle->import_data.fd;
  handle->meta_data_.gem_handles_[0] = gem_handle;
  handle->meta_data_.offsets_[0] = 0;
  if (handle->is_dumb_buffer)
    handle->meta_data_.pitches_[0] = handle->import_data.stride;
  else
    handle->meta_data_.pitches_[0] = gbm_bo_get_stride(handle->bo);
#endif

  return true;
}

uint32_t GbmBufferHandler::GetTotalPlanes(HWCNativeHandle handle) const {
  return handle->total_planes;
}

void *GbmBufferHandler::Map(HWCNativeHandle handle, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height, uint32_t *stride,
                            void **map_data, size_t plane) const {
  if (!handle->bo)
    return NULL;

#if USE_MINIGBM
  return gbm_bo_map(handle->bo, x, y, width, height, GBM_BO_TRANSFER_WRITE,
                    stride, map_data, plane);
#else
  return gbm_bo_map(handle->bo, x, y, width, height, GBM_BO_TRANSFER_WRITE,
                    stride, map_data);
#endif
}

int32_t GbmBufferHandler::UnMap(HWCNativeHandle handle, void *map_data) const {
  if (!handle->bo)
    return -1;

  gbm_bo_unmap(handle->bo, map_data);
  return 0;
}

}  // namespace hwcomposer
