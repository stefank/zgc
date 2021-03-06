/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zOopClosures.inline.hpp"
#include "gc/z/zStackWatermark.hpp"
#include "gc/z/zThread.inline.hpp"
#include "gc/z/zThreadLocalAllocBuffer.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "gc/z/zVerify.hpp"
#include "memory/resourceArea.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "utilities/preserveException.hpp"

ZOnStackCodeBlobClosure::ZOnStackCodeBlobClosure() :
    _bs_nm(BarrierSet::barrier_set()->barrier_set_nmethod()) {}

void ZOnStackCodeBlobClosure::do_code_blob(CodeBlob* cb) {
  nmethod* const nm = cb->as_nmethod_or_null();
  if (nm != NULL) {
    bool result = _bs_nm->nmethod_entry_barrier(nm);
    assert(result, "NMethod on-stack must be alive");
  }
}

ThreadLocalAllocStats& ZStackWatermark::stats() {
  return _stats;
}

uint32_t ZStackWatermark::epoch_id() const {
  return *ZAddressBadMaskHighOrderBitsAddr;
}

ZStackWatermark::ZStackWatermark(JavaThread* jt) :
    StackWatermark(jt, StackWatermarkKind::gc, *ZAddressBadMaskHighOrderBitsAddr),
    _jt_cl(),
    _cb_cl(),
    _stats() {}

OopClosure* ZStackWatermark::closure_from_context(void* context) {
  if (context != NULL) {
    assert(ZThread::is_worker(), "Unexpected thread passing in context: " PTR_FORMAT, p2i(context));
    return reinterpret_cast<OopClosure*>(context);
  } else {
    return &_jt_cl;
  }
}

void ZStackWatermark::start_processing_impl(void* context) {
  ZVerify::verify_thread_no_frames_bad(_jt);

  // Process the non-frame part of the thread
  _jt->oops_do_no_frames(closure_from_context(context), &_cb_cl);
  ZThreadLocalData::do_invisible_root(_jt, ZBarrier::load_barrier_on_invisible_root_oop_field);

  ZVerify::verify_thread_frames_bad(_jt);

  // Update thread local address bad mask
  ZThreadLocalData::set_address_bad_mask(_jt, ZAddressBadMask);

  // Retire TLAB
  if (ZGlobalPhase == ZPhaseMark) {
    ZThreadLocalAllocBuffer::retire(_jt, &_stats);
  } else {
    ZThreadLocalAllocBuffer::remap(_jt);
  }

  // Publishes the processing start to concurrent threads
  StackWatermark::start_processing_impl(context);
}

void ZStackWatermark::process(frame frame, RegisterMap& register_map, void* context) {
  ZVerify::verify_frame_bad(frame, register_map);
  frame.oops_do(closure_from_context(context), &_cb_cl, &register_map);
}
