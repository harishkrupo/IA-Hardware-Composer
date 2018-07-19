/*
// Copyright (c) 2017 Intel Corporation
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

#include "hwcutils.h"

#include <poll.h>

#include "hwctrace.h"

#include <drm_fourcc.h>

namespace hwcomposer {

int HWCPoll(int fd, int timeout) {
  CTRACE();
  int ret;
  struct pollfd fds[1];
  fds[0].fd = fd;
  fds[0].events = POLLIN;

  if ((ret = poll(fds, 1, timeout)) <= 0) {
    ETRACE("Poll Failed in HWCPoll %s", PRINTERROR());
  }
  return ret;
}

void ResetRectToRegion(const HwcRegion& hwc_region, HwcRect<int>& rect) {
  size_t total_rects = hwc_region.size();
  if (total_rects == 0) {
    rect.left = 0;
    rect.top = 0;
    rect.right = 0;
    rect.bottom = 0;
    return;
  }

  const HwcRect<int>& new_rect = hwc_region.at(0);
  rect.left = new_rect.left;
  rect.top = new_rect.top;
  rect.right = new_rect.right;
  rect.bottom = new_rect.bottom;

  for (uint32_t r = 1; r < total_rects; r++) {
    const HwcRect<int>& temp = hwc_region.at(r);
    rect.left = std::min(rect.left, temp.left);
    rect.top = std::min(rect.top, temp.top);
    rect.right = std::max(rect.right, temp.right);
    rect.bottom = std::max(rect.bottom, temp.bottom);
  }
}

void CalculateRect(const HwcRect<int>& target_rect, HwcRect<int>& new_rect) {
  if (new_rect.empty()) {
    new_rect = target_rect;
    return;
  }

  if (target_rect.empty()) {
    return;
  }

  new_rect.left = std::min(target_rect.left, new_rect.left);
  new_rect.top = std::min(target_rect.top, new_rect.top);
  new_rect.right = std::max(target_rect.right, new_rect.right);
  new_rect.bottom = std::max(target_rect.bottom, new_rect.bottom);
}

void CalculateSourceRect(const HwcRect<float>& target_rect,
                         HwcRect<float>& new_rect) {
  if (new_rect.empty()) {
    new_rect = target_rect;
    return;
  }

  if (target_rect.empty()) {
    return;
  }

  new_rect.left = std::min(target_rect.left, new_rect.left);
  new_rect.top = std::min(target_rect.top, new_rect.top);
  new_rect.right = std::max(target_rect.right, new_rect.right);
  new_rect.bottom = std::max(target_rect.bottom, new_rect.bottom);
}

bool IsSupportedMediaFormat(uint32_t format) {
  switch (format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_P010:
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YUV444:
    case DRM_FORMAT_UYVY:
    case DRM_FORMAT_YUYV:
    case DRM_FORMAT_YVYU:
    case DRM_FORMAT_VYUY:
    case DRM_FORMAT_AYUV:
    case DRM_FORMAT_NV12_Y_TILED_INTEL:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_YVU420_ANDROID:
      return true;
    default:
      break;
  }

  return false;
}

uint32_t GetTotalPlanesForFormat(uint32_t format) {
  switch (format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_P010:
      return 2;
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YUV444:
      return 3;
    case DRM_FORMAT_UYVY:
    case DRM_FORMAT_YUYV:
    case DRM_FORMAT_YVYU:
    case DRM_FORMAT_VYUY:
    case DRM_FORMAT_AYUV:
      return 1;
    default:
      break;
  }

  return 1;
}

std::string StringifyRect(HwcRect<int> rect) {
  std::stringstream ss;
  ss << "{(" << rect.left << "," << rect.top << ") "
     << "(" << rect.right << "," << rect.bottom << ")}";

  return ss.str();
}

std::string StringifyRegion(HwcRegion region) {
  std::stringstream ss;
  std::string separator = "";

  ss << "[";
  for (auto& rect : region) {
    ss << separator << StringifyRect(rect);
    separator = ", ";
  }
  ss << "]";

  return ss.str();
}

HwcRect<int> RotateRect(HwcRect<int> rect, int disp_width, int disp_height,
                        uint32_t transform) {
  HwcRect<int> display_frame;
  display_frame.left = 0;
  display_frame.top = 0;
  display_frame.right = disp_width;
  display_frame.bottom = disp_height;

  int ox = 0, oy = 0;
  HwcRect<int> rotated_rect;
  HwcRect<float> scaled_rect;

  scaled_rect.left = float(rect.left) / disp_width;
  scaled_rect.top = float(rect.top) / disp_height;
  scaled_rect.right = float(rect.right) / disp_width;
  scaled_rect.bottom = float(rect.bottom) / disp_height;

  if (transform == hwcomposer::HWCTransform::kTransform270) {
    ox = display_frame.left;
    oy = display_frame.bottom;
    rotated_rect.left = ox + scaled_rect.top * disp_width;
    rotated_rect.top = oy - scaled_rect.right * disp_height;
    rotated_rect.right = ox + scaled_rect.bottom * disp_width;
    rotated_rect.bottom = oy - scaled_rect.left * disp_height;
  } else if (transform == hwcomposer::HWCTransform::kTransform180) {
    ox = display_frame.right;
    oy = display_frame.bottom;
    rotated_rect.left = ox - scaled_rect.right * disp_width;
    rotated_rect.top = oy - scaled_rect.bottom * disp_height;
    rotated_rect.right = ox - scaled_rect.left * disp_width;
    rotated_rect.bottom = oy - scaled_rect.top * disp_height;
  } else if (transform & hwcomposer::HWCTransform::kTransform90) {
    if (transform & hwcomposer::HWCTransform::kReflectX) {
      ox = display_frame.left;
      oy = display_frame.top;
      rotated_rect.left = ox + scaled_rect.top * disp_width;
      rotated_rect.top = oy + scaled_rect.left * disp_height;
      rotated_rect.right = ox + scaled_rect.bottom * disp_width;
      rotated_rect.bottom = oy + scaled_rect.right * disp_height;
    } else if (transform & hwcomposer::HWCTransform::kReflectY) {
      ox = display_frame.right;
      oy = display_frame.bottom;
      rotated_rect.left = ox - scaled_rect.bottom * disp_width;
      rotated_rect.top = oy - scaled_rect.right * disp_height;
      rotated_rect.right = ox - scaled_rect.top * disp_width;
      rotated_rect.bottom = oy - scaled_rect.left * disp_height;
    } else {
      ox = display_frame.right;
      oy = display_frame.top;
      rotated_rect.left = ox - scaled_rect.bottom * disp_width;
      rotated_rect.top = oy + scaled_rect.left * disp_height;
      rotated_rect.right = ox - scaled_rect.top * disp_width;
      rotated_rect.bottom = oy + scaled_rect.right * disp_height;
    }
  } else if (transform == 0) {
    ox = display_frame.left;
    oy = display_frame.top;
    rotated_rect.left = ox + scaled_rect.left * disp_width;
    rotated_rect.top = oy + scaled_rect.top * disp_height;
    rotated_rect.right = ox + scaled_rect.right * disp_width;
    rotated_rect.bottom = oy + scaled_rect.bottom * disp_height;
  }

  return rotated_rect;
}

}  // namespace hwcomposer
