/*
 * Copyright (c) 2017 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "linux_frontend.h"
#include <commondrmutils.h>
#include <hwcrect.h>

class IAHWCVsyncCallback : public hwcomposer::VsyncCallback {
 public:
  IAHWCVsyncCallback(iahwc_callback_data_t data, iahwc_function_ptr_t hook)
      : data_(data), hook_(hook) {
  }

  void Callback(uint32_t display, int64_t timestamp) {
    if (hook_ != NULL) {
      auto hook = reinterpret_cast<IAHWC_PFN_VSYNC>(hook_);
      hook(data_, display, timestamp);
    }
  }

 private:
  iahwc_callback_data_t data_;
  iahwc_function_ptr_t hook_;
};

IAHWC::IAHWC() {
  getFunctionPtr = HookGetFunctionPtr;
  close = HookClose;
}

int32_t IAHWC::Init() {
  if (!device_.Initialize()) {
    fprintf(stderr, "Unable to initialize GPU DEVICE");
    return IAHWC_ERROR_NO_RESOURCES;
  }

  std::vector<hwcomposer::NativeDisplay*> displays = device_.GetAllDisplays();

  for (hwcomposer::NativeDisplay* display : displays) {
    displays_.emplace_back(new IAHWCDisplay());
    IAHWCDisplay* iahwc_display = displays_.back();
    iahwc_display->Init(display);
  }

  return IAHWC_ERROR_NONE;
}

int IAHWC::HookOpen(const iahwc_module_t* module, iahwc_device_t** device) {
  IAHWC* iahwc = new IAHWC();
  iahwc->Init();
  *device = iahwc;

  return IAHWC_ERROR_NONE;
}

iahwc_function_ptr_t IAHWC::HookGetFunctionPtr(iahwc_device_t* /* device */,
                                               int func_descriptor) {
  switch (func_descriptor) {
    case IAHWC_FUNC_GET_NUM_DISPLAYS:
      return ToHook<IAHWC_PFN_GET_NUM_DISPLAYS>(
          DeviceHook<int32_t, decltype(&IAHWC::GetNumDisplays),
                     &IAHWC::GetNumDisplays, int*>);
    case IAHWC_FUNC_REGISTER_CALLBACK:
      return ToHook<IAHWC_PFN_REGISTER_CALLBACK>(
          DeviceHook<int32_t, decltype(&IAHWC::RegisterCallback),
                     &IAHWC::RegisterCallback, int, uint32_t,
                     iahwc_callback_data_t, iahwc_function_ptr_t>);
    case IAHWC_FUNC_GET_DISPLAY_INFO:
      return ToHook<IAHWC_PFN_GET_DISPLAY_INFO>(
          DisplayHook<decltype(&IAHWCDisplay::GetDisplayInfo),
                      &IAHWCDisplay::GetDisplayInfo, uint32_t, int, int32_t*>);
    case IAHWC_FUNC_GET_DISPLAY_NAME:
      return ToHook<IAHWC_PFN_GET_DISPLAY_NAME>(
          DisplayHook<decltype(&IAHWCDisplay::GetDisplayName),
                      &IAHWCDisplay::GetDisplayName, uint32_t*, char*>);
    case IAHWC_FUNC_GET_DISPLAY_CONFIGS:
      return ToHook<IAHWC_PFN_GET_DISPLAY_CONFIGS>(
          DisplayHook<decltype(&IAHWCDisplay::GetDisplayConfigs),
                      &IAHWCDisplay::GetDisplayConfigs, uint32_t*, uint32_t*>);
    case IAHWC_FUNC_SET_DISPLAY_GAMMA:
      return ToHook<IAHWC_PFN_SET_DISPLAY_GAMMA>(
          DisplayHook<decltype(&IAHWCDisplay::SetDisplayGamma),
                      &IAHWCDisplay::SetDisplayGamma, float, float, float>);
    case IAHWC_FUNC_SET_DISPLAY_CONFIG:
      return ToHook<IAHWC_PFN_SET_DISPLAY_CONFIG>(
          DisplayHook<decltype(&IAHWCDisplay::SetDisplayConfig),
                      &IAHWCDisplay::SetDisplayConfig, uint32_t>);
    case IAHWC_FUNC_GET_DISPLAY_CONFIG:
      return ToHook<IAHWC_PFN_GET_DISPLAY_CONFIG>(
          DisplayHook<decltype(&IAHWCDisplay::GetDisplayConfig),
                      &IAHWCDisplay::GetDisplayConfig, uint32_t*>);
    case IAHWC_FUNC_PRESENT_DISPLAY:
      return ToHook<IAHWC_PFN_PRESENT_DISPLAY>(
          DisplayHook<decltype(&IAHWCDisplay::PresentDisplay),
                      &IAHWCDisplay::PresentDisplay, int32_t*>);
    case IAHWC_FUNC_CREATE_LAYER:
      return ToHook<IAHWC_PFN_CREATE_LAYER>(
          DisplayHook<decltype(&IAHWCDisplay::CreateLayer),
                      &IAHWCDisplay::CreateLayer, uint32_t*>);
    case IAHWC_FUNC_LAYER_SET_BO:
      return ToHook<IAHWC_PFN_LAYER_SET_BO>(
          LayerHook<decltype(&IAHWCLayer::SetBo), &IAHWCLayer::SetBo, gbm_bo*>);
    case IAHWC_FUNC_LAYER_SET_ACQUIRE_FENCE:
      return ToHook<IAHWC_PFN_LAYER_SET_ACQUIRE_FENCE>(
          LayerHook<decltype(&IAHWCLayer::SetAcquireFence),
                    &IAHWCLayer::SetAcquireFence, int32_t>);
  case IAHWC_FUNC_LAYER_SET_USAGE:
    return ToHook<IAHWC_PFN_LAYER_SET_USAGE>(
                                             LayerHook<decltype(&IAHWCLayer::SetLayerUsage),
                                             &IAHWCLayer::SetLayerUsage, int32_t>);
    case IAHWC_FUNC_INVALID:
    default:
      return NULL;
  }
}

int IAHWC::HookClose(iahwc_device_t* dev) {
  delete dev;
  return 0;
}

// private function implementations

int IAHWC::GetNumDisplays(int* num_displays) {
  *num_displays = 0;
  for (IAHWCDisplay* display : displays_) {
    if (display->IsConnected())
      *num_displays += 1;
  }

  return IAHWC_ERROR_NONE;
}

int IAHWC::RegisterCallback(int32_t description, uint32_t display_id,
                            iahwc_callback_data_t data,
                            iahwc_function_ptr_t hook) {
  switch (description) {
    case IAHWC_CALLBACK_VSYNC: {
      if (display_id >= displays_.size())
        return IAHWC_ERROR_BAD_DISPLAY;
      IAHWCDisplay* display = displays_.at(display_id);
      return display->RegisterVsyncCallback(data, hook);
    }
    default:
      return IAHWC_ERROR_BAD_PARAMETER;
  }
}

IAHWC::IAHWCDisplay::IAHWCDisplay() : native_display_(NULL) {
}

int IAHWC::IAHWCDisplay::Init(hwcomposer::NativeDisplay* display) {
  native_display_ = display;
}

int IAHWC::IAHWCDisplay::GetDisplayInfo(uint32_t config, int attribute,
                                        int32_t* value) {
  hwcomposer::HWCDisplayAttribute attrib =
      static_cast<hwcomposer::HWCDisplayAttribute>(attribute);

  bool ret = native_display_->GetDisplayAttribute(config, attrib, value);

  if (!ret)
    return IAHWC_ERROR_NO_RESOURCES;

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::GetDisplayName(uint32_t* size, char* name) {
  bool ret = native_display_->GetDisplayName(size, name);

  if (!ret)
    return IAHWC_ERROR_NO_RESOURCES;

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::GetDisplayConfigs(uint32_t* num_configs,
                                           uint32_t* configs) {
  bool ret = native_display_->GetDisplayConfigs(num_configs, configs);

  if (!ret)
    return IAHWC_ERROR_NO_RESOURCES;

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::SetDisplayGamma(float r, float b, float g) {
  native_display_->SetGamma(r, g, b);
  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::SetDisplayConfig(uint32_t config) {
  bool ret = native_display_->SetActiveConfig(config);

  if (!ret)
    return IAHWC_ERROR_NO_RESOURCES;

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::GetDisplayConfig(uint32_t* config) {
  bool ret = native_display_->GetActiveConfig(config);

  if (!ret)
    return IAHWC_ERROR_NO_RESOURCES;

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::PresentDisplay(int32_t* release_fd) {
  std::vector<hwcomposer::HwcLayer*> layers;

  for (auto [first, second] : layers_) {
    layers.emplace_back(second->GetLayer());
  }

  native_display_->Present(layers, release_fd);

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::CreateLayer(uint32_t* layer_handle) {

  *layer_handle = layers_.size();
  layers_.emplace(*layer_handle, new IAHWCLayer());

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCDisplay::RegisterVsyncCallback(iahwc_callback_data_t data,
                                               iahwc_function_ptr_t hook) {
  auto callback = std::make_shared<IAHWCVsyncCallback>(data, hook);
  native_display_->VSyncControl(true);
  int ret = native_display_->RegisterVsyncCallback(std::move(callback),
                                                   static_cast<int>(0));
  if (ret) {
    return IAHWC_ERROR_BAD_DISPLAY;
  }
  return IAHWC_ERROR_NONE;
}

bool IAHWC::IAHWCDisplay::IsConnected() {
  return native_display_->IsConnected();
}

IAHWC::IAHWCLayer::IAHWCLayer() {
  iahwc_layer_ = new hwcomposer::HwcLayer();
  hwc_handle_ = NULL;
  layer_usage_ = IAHWC_LAYER_USAGE_NORMAL;
}

int IAHWC::IAHWCLayer::SetBo(gbm_bo* bo) {
  int32_t width, height;

  if (hwc_handle_) {
    ::close(hwc_handle_->import_data.fd);
    delete hwc_handle_;
  }

  width = gbm_bo_get_width(bo);
  height = gbm_bo_get_height(bo);
  hwc_handle_ = new struct gbm_handle();
  hwc_handle_->import_data.width = width;
  hwc_handle_->import_data.height = height;
  hwc_handle_->import_data.format = gbm_bo_get_format(bo);
  hwc_handle_->import_data.fd = gbm_bo_get_fd(bo);
  hwc_handle_->import_data.stride = gbm_bo_get_stride(bo);
  hwc_handle_->total_planes =
      drm_bo_get_num_planes(hwc_handle_->import_data.format);
  hwc_handle_->bo = bo;
  hwc_handle_->hwc_buffer_ = true;
  hwc_handle_->gbm_flags = 0;

  iahwc_layer_->SetSourceCrop(hwcomposer::HwcRect<float>(0, 0, width, height));
  hwcomposer::HwcRect<int> display_frame(0, 0, width, height);
  iahwc_layer_->SetDisplayFrame(display_frame, 0);
  std::vector<hwcomposer::HwcRect<int>> damage_region;
  damage_region.emplace_back(display_frame);
  iahwc_layer_->SetSurfaceDamage(damage_region);
  iahwc_layer_->SetNativeHandle(hwc_handle_);

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCLayer::SetAcquireFence(int32_t acquire_fence) {
  iahwc_layer_->SetAcquireFence(acquire_fence);

  return IAHWC_ERROR_NONE;
}

int IAHWC::IAHWCLayer::SetLayerUsage(int32_t layer_usage) {
  layer_usage_ = layer_usage;

  return IAHWC_ERROR_NONE;
}

hwcomposer::HwcLayer* IAHWC::IAHWCLayer::GetLayer() {
  return iahwc_layer_;
}

iahwc_module_t IAHWC_MODULE_INFO = {
    .name = "IA Hardware Composer",
    .open = IAHWC::HookOpen,
};
