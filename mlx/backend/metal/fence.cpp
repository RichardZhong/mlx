// Copyright © 2024 Apple Inc.

#include "mlx/fence.h"
#include "mlx/backend/metal/device.h"
#include "mlx/backend/metal/metal_impl.h"
#include "mlx/scheduler.h"
#include "mlx/utils.h"

namespace mlx::core {

Fence::Fence() {
  auto d = metal::device(Device::gpu).mtl_device();
  if (!d->supportsFamily(MTL::GPUFamilyMetal3)) {
    use_fast_ = false;
  } else if (__builtin_available(macOS 15, iOS 18, *)) {
    use_fast_ = env::metal_fast_synch();
  }
  if (!use_fast_) {
    // Wraps Metal SharedEvent
    auto dtor = [](void* ptr) {
      auto p = metal::new_scoped_memory_pool();
      static_cast<MTL::SharedEvent*>(ptr)->release();
    };
    auto p = metal::new_scoped_memory_pool();
    fence_ = std::shared_ptr<void>(d->newSharedEvent(), dtor);
  } else {
    auto dtor = [](void* buf) {
      allocator::free(static_cast<MTL::Buffer*>(buf));
    };
    auto buf = allocator::malloc_or_wait(sizeof(uint32_t)).ptr();
    fence_ = std::shared_ptr<void>(buf, dtor);
    cpu_value()[0] = 0;
  }
}

void Fence::wait(Stream stream, const array& array) {
  if (stream.device == Device::cpu) {
    scheduler::enqueue(stream, [*this]() mutable {
      if (!use_fast_) {
        if (!static_cast<MTL::SharedEvent*>(fence_.get())
                 ->waitUntilSignaledValue(count_, -1)) {
          throw std::runtime_error("[Event::wait] Timed out");
        }
        return;
      }
      while (cpu_value()[0] < count_) {
      }
    });
    return;
  }

  auto& d = metal::device(stream.device);
  auto idx = stream.index;

  if (!use_fast_) {
    d.end_encoding(idx);
    auto command_buffer = d.get_command_buffer(idx);
    command_buffer->encodeWait(static_cast<MTL::Event*>(fence_.get()), count_);
    command_buffer->addCompletedHandler(
        [fence_ = fence_](MTL::CommandBuffer* cbuf) {});
    return;
  }

  auto& compute_encoder = d.get_command_encoder(idx);

  // Register outputs to ensure that no kernels which depends on the
  // output starts before this one is done
  compute_encoder.register_output_array(array);

  auto kernel = d.get_kernel("fence_wait");
  MTL::Size kernel_dims = MTL::Size(1, 1, 1);
  compute_encoder.set_compute_pipeline_state(kernel);

  auto buf = static_cast<MTL::Buffer*>(fence_.get());
  compute_encoder.set_buffer(buf, 0);
  compute_encoder.set_bytes(count_, 1);
  compute_encoder.dispatch_threads(kernel_dims, kernel_dims);

  d.get_command_buffer(idx)->addCompletedHandler(
      [fence = fence_](MTL::CommandBuffer* cbuf) {});
}

void Fence::update(Stream stream, const std::vector<array>& arrays) {
  count_++;

  if (stream.device == Device::cpu) {
    scheduler::enqueue(stream, [*this]() mutable {
      if (!use_fast_) {
        static_cast<MTL::SharedEvent*>(fence_.get())->setSignaledValue(count_);
        return;
      }

      cpu_value()[0] = count_;
    });
    return;
  }

  auto& d = metal::device(stream.device);
  auto idx = stream.index;

  if (!use_fast_) {
    d.end_encoding(idx);
    auto command_buffer = d.get_command_buffer(idx);
    command_buffer->encodeSignalEvent(
        static_cast<MTL::Event*>(fence_.get()), count_);
    command_buffer->addCompletedHandler(
        [fence_ = fence_](MTL::CommandBuffer* cbuf) {});
    return;
  }

  // Launch input visibility kernels
  auto& compute_encoder = d.get_command_encoder(idx);
  auto kernel = d.get_kernel("input_coherent");
  for (auto& x : arrays) {
    uint32_t nthreads = (x.data_size() * x.itemsize() + sizeof(uint32_t) - 1) /
        sizeof(uint32_t);
    MTL::Size group_dims = MTL::Size(1024, 1, 1);
    MTL::Size grid_dims = MTL::Size((nthreads + 1024 - 1) / 1024, 1, 1);
    compute_encoder.set_compute_pipeline_state(kernel);
    compute_encoder.set_input_array(x, 0);
    compute_encoder.set_bytes(nthreads, 1);
    compute_encoder.dispatch_threadgroups(group_dims, grid_dims);
  }

  // Barrier on previous kernels
  compute_encoder.barrier();

  // Launch value update kernel
  kernel = d.get_kernel("fence_update");
  MTL::Size kernel_dims = MTL::Size(1, 1, 1);
  compute_encoder.set_compute_pipeline_state(kernel);

  auto buf = static_cast<MTL::Buffer*>(fence_.get());
  compute_encoder.set_buffer(buf, 0);
  compute_encoder.set_bytes(count_, 1);
  compute_encoder.dispatch_threads(kernel_dims, kernel_dims);

  d.get_command_buffer(idx)->addCompletedHandler(
      [fence = fence_](MTL::CommandBuffer* cbuf) {});
}

std::atomic_uint* Fence::cpu_value() {
  return static_cast<std::atomic_uint*>(
      static_cast<MTL::Buffer*>(fence_.get())->contents());
}

} // namespace mlx::core
