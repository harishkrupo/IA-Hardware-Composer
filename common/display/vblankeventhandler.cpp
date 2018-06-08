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

#include "vblankeventhandler.h"

#include <stdlib.h>
#include <time.h>

#include "displayqueue.h"
#include "hwctrace.h"

namespace hwcomposer {

static const int64_t kOneSecondNs = 1 * 1000 * 1000 * 1000;

VblankEventHandler::VblankEventHandler(DisplayQueue* queue)
    : HWCThread(-8, "VblankEventHandler"),
      display_(0),
      enabled_(false),
      fd_(-1),
      last_timestamp_(-1),
      queue_(queue) {
  memset(&type_, 0, sizeof(type_));
}

VblankEventHandler::~VblankEventHandler() {
}

void VblankEventHandler::Init(int fd, int pipe) {
  fd_ = fd;
  uint32_t high_crtc = (pipe << DRM_VBLANK_HIGH_CRTC_SHIFT);
  type_ = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE |
                             (high_crtc & DRM_VBLANK_HIGH_CRTC_MASK));
  fd_handler_.AddFd(fd_);
}

bool VblankEventHandler::SetPowerMode(uint32_t power_mode) {
  if (power_mode != kOn) {
    Exit();
  } else {
    if (!InitWorker()) {
      ETRACE("Failed to initalize thread for VblankEventHandler. %s",
             PRINTERROR());
    }
  }

  return true;
}

int VblankEventHandler::RegisterCallback(
    std::shared_ptr<VsyncCallback> callback, uint32_t display) {
  spin_lock_.lock();
  callback_ = callback;
  display_ = display;
  last_timestamp_ = -1;
  spin_lock_.unlock();
  return 0;
}

int VblankEventHandler::VSyncControl(bool enabled) {
  IPAGEFLIPEVENTTRACE("VblankEventHandler VSyncControl enabled %d", enabled);
  if (enabled_ == enabled)
    return 0;

  spin_lock_.lock();
  enabled_ = enabled;
  last_timestamp_ = -1;
  spin_lock_.unlock();

  return 0;
}

void VblankEventHandler::HandlePageFlipEvent(unsigned int sec,
                                             unsigned int usec) {
  fprintf(stderr, "hkps %s:%d\n", __PRETTY_FUNCTION__, __LINE__);
  int64_t timestamp = ((int64_t)sec * kOneSecondNs) + ((int64_t)usec * 1000);
  IPAGEFLIPEVENTTRACE("HandleVblankCallBack Frame Time %f",
                      static_cast<float>(timestamp - last_timestamp_) / (1000));
  last_timestamp_ = timestamp;

  IPAGEFLIPEVENTTRACE("Callback called from HandlePageFlipEvent. %lu",
                      timestamp);
  spin_lock_.lock();
  if (enabled_ && callback_) {
    callback_->Callback(display_, timestamp);
  }
  spin_lock_.unlock();
}

void VblankEventHandler::HandleWait() {
  if (fd_handler_.Poll(-1) <= 0) {
    ETRACE("Poll Failed in DisplayManager %s", PRINTERROR());
  }
}

  static void
  PageFlipHandler2(int fd, unsigned int frame, unsigned int sec,
                  unsigned int usec, unsigned int crtc_id, void *data) {

    fprintf(stderr, "hkps %s:%d\n", __PRETTY_FUNCTION__, __LINE__);
    VblankEventHandler* vbl = (VblankEventHandler*) data;
    vbl->HandlePageFlipEvent(sec, usec);
  }

  static void
  PageFlipHandler(int fd, unsigned int frame, unsigned int sec,
                  unsigned int usec, void *data) {

    fprintf(stderr, "hkps %s:%d\n", __PRETTY_FUNCTION__, __LINE__);
    VblankEventHandler* vbl = (VblankEventHandler*) data;
    vbl->HandlePageFlipEvent(sec, usec);
  }

void VblankEventHandler::HandleRoutine() {
  queue_->HandleIdleCase();

  fprintf(stderr, "hkps %s:%d\n", __PRETTY_FUNCTION__, __LINE__);
  if (fd_handler_.IsReady(fd_)) {
    fprintf(stderr, "hkps %s:%d\n", __PRETTY_FUNCTION__, __LINE__);
    drmEventContext evctx;
    memset(&evctx, 0, sizeof evctx);
    evctx.version = 3;
    evctx.page_flip_handler2 = PageFlipHandler2;
    evctx.page_flip_handler = PageFlipHandler;
    evctx.vblank_handler = NULL;
    drmHandleEvent(fd_, &evctx);
  }

  drmVBlank vblank;
  memset(&vblank, 0, sizeof(vblank));
  vblank.request.sequence = 1;

  int fd = fd_;
  vblank.request.type = type_;

  int ret = drmWaitVBlank(fd, &vblank);
  // if (!ret)
  //   HandlePageFlipEvent(vblank.reply.tval_sec, (int64_t)vblank.reply.tval_usec);
}

}  // namespace hwcomposer
