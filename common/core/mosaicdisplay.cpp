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

#include "mosaicdisplay.h"

#include <map>
#include <sstream>
#include <string>

#include <hwclayer.h>

#include "hwctrace.h"

namespace hwcomposer {

class MDVsyncCallback : public hwcomposer::VsyncCallback {
 public:
  MDVsyncCallback(MosaicDisplay *display) : display_(display) {
  }

  void Callback(uint32_t /*display*/, int64_t timestamp) {
    display_->VSyncUpdate(timestamp);
  }

 private:
  MosaicDisplay *display_;
};

class MDRefreshCallback : public hwcomposer::RefreshCallback {
 public:
  MDRefreshCallback(MosaicDisplay *display) : display_(display) {
  }

  void Callback(uint32_t /*display*/) {
    display_->RefreshUpdate();
  }

 private:
  MosaicDisplay *display_;
};

class MDHotPlugCallback : public hwcomposer::HotPlugCallback {
 public:
  MDHotPlugCallback(MosaicDisplay *display) : display_(display) {
  }

  void Callback(uint32_t /*display*/, bool connected) {
    display_->HotPlugUpdate(connected);
  }

 private:
  MosaicDisplay *display_;
};

MosaicDisplay::MosaicDisplay(const std::vector<NativeDisplay *> &displays)
    : dpix_(0), dpiy_(0) {
  uint32_t size = displays.size();
  physical_displays_.reserve(size);
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.emplace_back(displays.at(i));
  }
}

MosaicDisplay::~MosaicDisplay() {
}

bool MosaicDisplay::Initialize(NativeBufferHandler * /*buffer_handler*/,
                               FrameBufferManager * /*frame_buffer_manager*/) {
  return true;
}

bool MosaicDisplay::IsConnected() const {
  uint32_t size = physical_displays_.size();
  bool connected = false;
  for (uint32_t i = 0; i < size; i++) {
    if (physical_displays_.at(i)->IsConnected()) {
      connected = true;
      break;
    }
  }

  return connected;
}

uint32_t MosaicDisplay::Width() const {
  return width_;
}

uint32_t MosaicDisplay::Height() const {
  return height_;
}

uint32_t MosaicDisplay::PowerMode() const {
  return power_mode_;
}

int MosaicDisplay::GetDisplayPipe() {
  return physical_displays_.at(0)->GetDisplayPipe();
}

bool MosaicDisplay::SetActiveConfig(uint32_t config) {
  config_ = config;
  width_ = 0;
  height_ = 0;
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetActiveConfig(config_);
  }

  uint32_t avg = 0;
  int32_t previous_refresh = 0;
  for (uint32_t i = 0; i < size; i++) {
    int32_t dpix = 0;
    int32_t dpiy = 0;
    int32_t refresh = 0;
    height_ = std::max(height_, physical_displays_.at(i)->Height());
    width_ += physical_displays_.at(i)->Width();
    physical_displays_.at(i)->GetDisplayAttribute(
        config_, HWCDisplayAttribute::kDpiX, &dpix);
    physical_displays_.at(i)->GetDisplayAttribute(
        config_, HWCDisplayAttribute::kDpiY, &dpiy);
    physical_displays_.at(i)->GetDisplayAttribute(
        config_, HWCDisplayAttribute::kRefreshRate, &refresh);
    dpix_ += dpix;
    dpiy_ += dpiy;
    refresh_ += refresh;
    if (previous_refresh < refresh)
      preferred_display_index_ = i;

    avg++;
  }

  if (avg > 0) {
    refresh_ /= avg;
    dpix_ /= avg;
    dpiy_ /= avg;
  }

  return true;
}

bool MosaicDisplay::GetActiveConfig(uint32_t *config) {
  if (!config)
    return false;

  *config = config_;
  return true;
}

bool MosaicDisplay::SetPowerMode(uint32_t power_mode) {
  power_mode_ = power_mode;
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetPowerMode(power_mode);
  }

  return true;
}

bool MosaicDisplay::Present(std::vector<HwcLayer *> &source_layers,
                            int32_t *retire_fence,
                            PixelUploaderCallback *call_back,
                            bool /*handle_constraints*/) {
  if (power_mode_ != kOn)
    return true;

  ALOGE("hkps %s:%d \n", __PRETTY_FUNCTION__, __LINE__);
  lock_.lock();
  if (update_connected_displays_) {
    uint32_t size = physical_displays_.size();
    int32_t previous_refresh = 0;
    for (uint32_t i = 0; i < size; i++) {
      NativeDisplay *nd = physical_displays_.at(i);
      if (nd->IsConnected()) {
        if (mosaic_presenters_.find(nd) == mosaic_presenters_.end()) {
          mosaic_presenters_.emplace(std::piecewise_construct,
                                     std::forward_as_tuple(nd),
                                     std::forward_as_tuple());
          MosaicDisplayPresenter &mdp = mosaic_presenters_.at(nd);
          mdp.Initialize();
        }

        int32_t refresh = 0;
        physical_displays_.at(i)->GetDisplayAttribute(
          config_, HWCDisplayAttribute::kRefreshRate, &refresh);
        if (previous_refresh < refresh)
          preferred_display_index_ = i;
      } else {
        if (mosaic_presenters_.find(nd) != mosaic_presenters_.end()) {
          MosaicDisplayPresenter &mdp = mosaic_presenters_.at(nd);
          mdp.ExitThread();
          mosaic_presenters_.erase(nd);
        }
      }
    }
    update_connected_displays_ = false;
  }
  lock_.unlock();

  uint32_t size = mosaic_presenters_.size();
  int32_t left_constraint = 0;
  int32_t display_id = 0;

  for (auto &l : mosaic_presenters_) {
    NativeDisplay *display = l.first;
    MosaicDisplayPresenter &mdp = l.second;
    mdp.Present(display, left_constraint, size, display_id, &source_layers,
                call_back);
    left_constraint = left_constraint + display->Width();
    display_id++;
    mdp.Wait();

    if (display_id > 0)
      break;
  }

  *retire_fence = -1;


  return true;
}

bool MosaicDisplay::PresentClone(NativeDisplay * /*display*/) {
  return false;
}

int MosaicDisplay::RegisterVsyncCallback(
    std::shared_ptr<VsyncCallback> callback, uint32_t display_id) {
  display_id_ = display_id;
  vsync_callback_ = callback;

  uint32_t size = physical_displays_.size();
  auto v_callback = std::make_shared<MDVsyncCallback>(this);
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->RegisterVsyncCallback(
        v_callback, physical_displays_.at(i)->GetDisplayPipe());
  }

  return 0;
}

void MosaicDisplay::RegisterRefreshCallback(
    std::shared_ptr<RefreshCallback> callback, uint32_t display_id) {
  display_id_ = display_id;
  refresh_callback_ = callback;

  uint32_t size = physical_displays_.size();
  auto r_callback = std::make_shared<MDRefreshCallback>(this);
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->RegisterRefreshCallback(
        r_callback, physical_displays_.at(i)->GetDisplayPipe());
  }
}

void MosaicDisplay::RegisterHotPlugCallback(
    std::shared_ptr<HotPlugCallback> callback, uint32_t display_id) {
  lock_.lock();
  display_id_ = display_id;
  hotplug_callback_ = callback;
  lock_.unlock();

  uint32_t size = physical_displays_.size();
  auto h_callback = std::make_shared<MDHotPlugCallback>(this);
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->RegisterHotPlugCallback(
        h_callback, physical_displays_.at(i)->GetDisplayPipe());
  }
}

void MosaicDisplay::VSyncControl(bool enabled) {
  if (enable_vsync_ == enabled)
    return;

  enable_vsync_ = enabled;
  vsync_timestamp_ = 0;
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->VSyncControl(enabled);
  }
}

void MosaicDisplay::VSyncUpdate(int64_t timestamp) {
  lock_.lock();
  if (vsync_callback_ && enable_vsync_ && vsync_divisor_ > 0) {
    vsync_counter_--;
    vsync_timestamp_ += timestamp;
    if (vsync_counter_ == 0) {
      vsync_timestamp_ /= vsync_divisor_;
      vsync_callback_->Callback(display_id_, vsync_timestamp_);
      vsync_counter_ = vsync_divisor_;
      vsync_timestamp_ = 0;
      pending_vsync_ = false;
    } else {
      pending_vsync_ = true;
    }
  }

  lock_.unlock();
}

void MosaicDisplay::RefreshUpdate() {
  if (connected_ && refresh_callback_ && power_mode_ == kOn) {
    refresh_callback_->Callback(display_id_);
  }
}

void MosaicDisplay::HotPlugUpdate(bool connected) {
  lock_.lock();
  update_connected_displays_ = true;
  uint32_t total_connected_displays = 0;
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    if (physical_displays_.at(i)->IsConnected()) {
      total_connected_displays++;
    }
  }

  if (vsync_callback_ && enable_vsync_ && pending_vsync_ &&
      (total_connected_displays > 0)) {
    if (vsync_counter_ == total_connected_displays) {
      vsync_timestamp_ /= total_connected_displays;
      vsync_callback_->Callback(display_id_, vsync_timestamp_);
      pending_vsync_ = false;
    }
  }

  vsync_counter_ = total_connected_displays;
  vsync_divisor_ = vsync_counter_;

  if (connected_ == connected) {
    lock_.unlock();
    return;
  }

  if (hotplug_callback_) {
    if (!connected && connected_ && total_connected_displays) {
      lock_.unlock();
      return;
    }

    connected_ = connected;
    hotplug_callback_->Callback(display_id_, connected);
  }
  lock_.unlock();
}

bool MosaicDisplay::CheckPlaneFormat(uint32_t format) {
  return physical_displays_.at(0)->CheckPlaneFormat(format);
}

void MosaicDisplay::SetGamma(float red, float green, float blue) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetGamma(red, green, blue);
  }
}

void MosaicDisplay::SetContrast(uint32_t red, uint32_t green, uint32_t blue) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetContrast(red, green, blue);
  }
}

void MosaicDisplay::SetBrightness(uint32_t red, uint32_t green, uint32_t blue) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetBrightness(red, green, blue);
  }
}

void MosaicDisplay::SetExplicitSyncSupport(bool disable_explicit_sync) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetExplicitSyncSupport(disable_explicit_sync);
  }
}

void MosaicDisplay::SetVideoScalingMode(uint32_t mode) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetVideoScalingMode(mode);
  }
}

void MosaicDisplay::SetVideoColor(HWCColorControl color, float value) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetVideoColor(color, value);
  }
}

void MosaicDisplay::GetVideoColor(HWCColorControl color, float *value,
                                  float *start, float *end) {
  physical_displays_.at(0)->GetVideoColor(color, value, start, end);
}

void MosaicDisplay::RestoreVideoDefaultColor(HWCColorControl color) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->RestoreVideoDefaultColor(color);
  }
}

void MosaicDisplay::SetVideoDeinterlace(HWCDeinterlaceFlag flag,
                                        HWCDeinterlaceControl mode) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetVideoDeinterlace(flag, mode);
  }
}

void MosaicDisplay::RestoreVideoDefaultDeinterlace() {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->RestoreVideoDefaultDeinterlace();
  }
}

void MosaicDisplay::UpdateScalingRatio(uint32_t /*primary_width*/,
                                       uint32_t /*primary_height*/,
                                       uint32_t /*display_width*/,
                                       uint32_t /*display_height*/) {
}

void MosaicDisplay::CloneDisplay(NativeDisplay * /*source_display*/) {
}

bool MosaicDisplay::GetDisplayAttribute(uint32_t /*config*/,
                                        HWCDisplayAttribute attribute,
                                        int32_t *value) {
  bool status = true;
  switch (attribute) {
    case HWCDisplayAttribute::kWidth:
      *value = width_;
      break;
    case HWCDisplayAttribute::kHeight:
      *value = height_;
      break;
    case HWCDisplayAttribute::kRefreshRate:
      *value = refresh_;
      break;
    case HWCDisplayAttribute::kDpiX:
      *value = dpix_;
      break;
    case HWCDisplayAttribute::kDpiY:
      *value = dpiy_;
      break;
    default:
      *value = -1;
      status = false;
  }

  return status;
}

bool MosaicDisplay::GetDisplayConfigs(uint32_t *num_configs,
                                      uint32_t *configs) {
  *num_configs = 1;
  if (configs) {
    configs[0] = 0;
  }
  return true;
}

bool MosaicDisplay::GetDisplayName(uint32_t *size, char *name) {
  std::ostringstream stream;
  stream << "Mosaic";
  std::string string = stream.str();
  size_t length = string.length();
  if (!name) {
    *size = length;
    return true;
  }

  *size = std::min<uint32_t>(static_cast<uint32_t>(length - 1), *size);
  strncpy(name, string.c_str(), *size);
  return true;
}

void MosaicDisplay::SetHDCPState(HWCContentProtection state,
                                 HWCContentType content_type) {
  uint32_t size = physical_displays_.size();
  for (uint32_t i = 0; i < size; i++) {
    physical_displays_.at(i)->SetHDCPState(state, content_type);
  }
}

MosaicDisplay::MosaicDisplayPresenter::MosaicDisplayPresenter()
    : HWCThread(-8, "MosaicDisplayPresenter") {
  if (!cevent_.Initialize())
    return;

  fd_chandler_.AddFd(cevent_.get_fd());
}

MosaicDisplay::MosaicDisplayPresenter::~MosaicDisplayPresenter() {
}

bool MosaicDisplay::MosaicDisplayPresenter::Initialize() {
  if (!InitWorker()) {
    ETRACE("Failed to initalize MosaicDisplayPresenter. %s", PRINTERROR());
  }
  return true;
}

void MosaicDisplay::MosaicDisplayPresenter::Present(
    NativeDisplay *display, int32_t left_constraint, int32_t total_displays,
    int32_t display_id, std::vector<HwcLayer *> *source_layers,
    PixelUploaderCallback *call_back) {
  native_display_ = display;
  left_constraint_ = left_constraint;
  total_displays_ = total_displays;
  id_ = display_id;
  source_layers_ = source_layers;
  callback_ = call_back;

  HandleRoutine();
  // Resume();
}

  void MosaicDisplay::MosaicDisplayPresenter::ClearLayers() {
    uint32_t i;
    for (i = 0; i < layers_.size(); i++) {
      // delete layers_.at(i);
    }

    std::vector<HwcLayer*>().swap(layers_);
  }

void MosaicDisplay::MosaicDisplayPresenter::HandleRoutine() {

  ClearLayers();
  int32_t right_constraint = left_constraint_ + native_display_->Width();
  uint32_t dlconstraint =
      native_display_->GetLogicalIndex() * native_display_->Width();
  uint32_t drconstraint = dlconstraint + native_display_->Width();
  IMOSAICDISPLAYTRACE("Display index %d \n", id_);
  IMOSAICDISPLAYTRACE("dlconstraint %d \n", dlconstraint);
  IMOSAICDISPLAYTRACE("drconstraint %d \n", drconstraint);
  IMOSAICDISPLAYTRACE("right_constraint %d \n", right_constraint);
  IMOSAICDISPLAYTRACE("left_constraint %d \n", left_constraint_);

  size_t total_layers = source_layers_->size();

  for (size_t j = 0; j < total_layers; j++) {
    HwcLayer *source_layer = source_layers_->at(j);
    const HwcRect<int> &frame_Rect = source_layer->GetDisplayFrame();
    if ((frame_Rect.right < left_constraint_) ||
        (frame_Rect.left > right_constraint)) {
      continue;
    }

    HwcLayer *layer = source_layer;

    layer->SetLeftConstraint(dlconstraint);
    layer->SetRightConstraint(drconstraint);
    layer->SetLeftSourceConstraint(left_constraint_);
    layer->SetRightSourceConstraint(right_constraint);
    layer->SetTotalDisplays(total_displays_ - id_);

    layers_.emplace_back(layer);
  }

  ALOGE("hkps %s:%d layers size %d\n", __PRETTY_FUNCTION__, __LINE__, layers_.size());
  if (layers_.empty()) {
    return;
  }

  int32_t fence = -1;
  native_display_->Present(layers_, &fence, callback_, true);

  if (release_fence_ > 0)
    close(release_fence_);
  release_fence_ = fence;

  IMOSAICDISPLAYTRACE("Present called for Display index %d \n", id_);
  cevent_.Signal();
}

void MosaicDisplay::MosaicDisplayPresenter::ExitThread() {
  HWCThread::Exit();
  ClearLayers();
  if (release_fence_ > 0)
    close(release_fence_);
}

void MosaicDisplay::MosaicDisplayPresenter::Wait() {
  if (fd_chandler_.Poll(-1) <= 0) {
    ETRACE("Poll Failed in DisplayManager %s", PRINTERROR());
    return;
  }

  if (fd_chandler_.IsReady(cevent_.get_fd())) {
    // If eventfd_ is ready, we need to wait on it (using read()) to clean
    // the flag that says it is ready.
    cevent_.Wait();
  }
}

}  // namespace hwcomposer
