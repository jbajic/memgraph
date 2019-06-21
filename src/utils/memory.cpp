#include "utils/memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <glog/logging.h>

namespace utils {

// MonotonicBufferResource

namespace {

size_t GrowMonotonicBuffer(size_t current_size, size_t max_size) {
  double next_size = current_size * 1.34;
  if (next_size >= static_cast<double>(max_size)) {
    // Overflow, clamp to max_size
    return max_size;
  }
  return std::ceil(next_size);
}

}  // namespace

MonotonicBufferResource::MonotonicBufferResource(size_t initial_size)
    : initial_size_(initial_size) {}

MonotonicBufferResource::MonotonicBufferResource(size_t initial_size,
                                                 MemoryResource *memory)
    : memory_(memory), initial_size_(initial_size) {}

MonotonicBufferResource::MonotonicBufferResource(void *buffer,
                                                 size_t buffer_size,
                                                 MemoryResource *memory)
    : memory_(memory),
      initial_buffer_(buffer),
      initial_size_(buffer_size),
      next_buffer_size_(GrowMonotonicBuffer(
          initial_size_, std::numeric_limits<size_t>::max() - sizeof(Buffer))) {
}

MonotonicBufferResource::MonotonicBufferResource(
    MonotonicBufferResource &&other) noexcept
    : memory_(other.memory_),
      current_buffer_(other.current_buffer_),
      initial_buffer_(other.initial_buffer_),
      initial_size_(other.initial_size_),
      allocated_(other.allocated_) {
  other.current_buffer_ = nullptr;
}

MonotonicBufferResource &MonotonicBufferResource::operator=(
    MonotonicBufferResource &&other) noexcept {
  if (this == &other) return *this;
  Release();
  memory_ = other.memory_;
  current_buffer_ = other.current_buffer_;
  initial_buffer_ = other.initial_buffer_;
  initial_size_ = other.initial_size_;
  allocated_ = other.allocated_;
  other.current_buffer_ = nullptr;
  other.allocated_ = 0U;
  return *this;
}

void MonotonicBufferResource::Release() {
  for (auto *b = current_buffer_; b;) {
    auto *next = b->next;
    auto capacity = b->capacity;
    b->~Buffer();
    memory_->Deallocate(b, sizeof(*b) + capacity);
    b = next;
  }
  current_buffer_ = nullptr;
  allocated_ = 0U;
}

void *MonotonicBufferResource::DoAllocate(size_t bytes, size_t alignment) {
  static_assert(std::is_same_v<size_t, uintptr_t>);
  if (alignment > alignof(std::max_align_t))
    throw BadAlloc(
        "Alignment greater than alignof(std::max_align_t) is unsupported");

  auto push_current_buffer = [this, bytes](size_t next_size) {
    // Set capacity so that the bytes fit.
    size_t capacity = next_size > bytes ? next_size : bytes;
    // Handle the case when we need to align `Buffer::data` to a greater
    // `alignment`. We will simply always allocate with
    // alignof(std::max_align_t), and `sizeof(Buffer)` needs to be a multiple of
    // that to keep `data` correctly aligned.
    static_assert(sizeof(Buffer) % alignof(std::max_align_t) == 0);
    size_t alloc_size = sizeof(Buffer) + capacity;
    if (alloc_size <= capacity) throw BadAlloc("Allocation size overflow");
    void *ptr = memory_->Allocate(alloc_size);
    current_buffer_ = new (ptr) Buffer{current_buffer_, capacity};
    allocated_ = 0;
  };

  char *data = nullptr;
  size_t data_capacity = 0U;
  if (current_buffer_) {
    data = current_buffer_->data();
    data_capacity = current_buffer_->capacity;
  } else if (initial_buffer_) {
    data = reinterpret_cast<char *>(initial_buffer_);
    data_capacity = initial_size_;
  } else {  // missing current_buffer_ and initial_buffer_
    push_current_buffer(initial_size_);
    data = current_buffer_->data();
    data_capacity = current_buffer_->capacity;
  }
  char *buffer_head = data + allocated_;
  void *aligned_ptr = buffer_head;
  size_t available = data_capacity - allocated_;
  if (!std::align(alignment, bytes, aligned_ptr, available)) {
    // Not enough memory, so allocate a new block with aligned data.
    push_current_buffer(next_buffer_size_);
    aligned_ptr = buffer_head = data = current_buffer_->data();
    next_buffer_size_ = GrowMonotonicBuffer(
        next_buffer_size_, std::numeric_limits<size_t>::max() - sizeof(Buffer));
  }
  if (reinterpret_cast<char *>(aligned_ptr) < buffer_head)
    throw BadAlloc("Allocation alignment overflow");
  if (reinterpret_cast<char *>(aligned_ptr) + bytes <= aligned_ptr)
    throw BadAlloc("Allocation size overflow");
  allocated_ = reinterpret_cast<char *>(aligned_ptr) - data + bytes;
  return aligned_ptr;
}

// MonotonicBufferResource END

// PoolResource
//
// Implementation is partially based on "Small Object Allocation" implementation
// from "Modern C++ Design" by Andrei Alexandrescu. While some other parts are
// based on `libstdc++-9.1` implementation.

namespace impl {

Pool::Pool(size_t block_size, unsigned char blocks_per_chunk,
           MemoryResource *memory)
    : blocks_per_chunk_(blocks_per_chunk),
      block_size_(block_size),
      chunks_(memory) {}

Pool::~Pool() {
  CHECK(chunks_.empty()) << "You need to call Release before destruction!";
}

void *Pool::Allocate() {
  auto allocate_block_from_chunk = [this](Chunk *chunk) {
    unsigned char *available_block =
        chunk->data + (chunk->first_available_block_ix * block_size_);
    // Update free-list pointer (index in our case) by reading "next" from the
    // available_block.
    chunk->first_available_block_ix = *available_block;
    --chunk->blocks_available;
    return available_block;
  };
  if (last_alloc_chunk_ && last_alloc_chunk_->blocks_available > 0U)
    return allocate_block_from_chunk(last_alloc_chunk_);
  // Find a Chunk with available memory.
  for (auto &chunk : chunks_) {
    if (chunk.blocks_available > 0U) {
      last_alloc_chunk_ = &chunk;
      return allocate_block_from_chunk(last_alloc_chunk_);
    }
  }
  // We haven't found a Chunk with available memory, so allocate a new one.
  if (block_size_ > std::numeric_limits<size_t>::max() / blocks_per_chunk_)
    throw BadAlloc("Allocation size overflow");
  size_t data_size = blocks_per_chunk_ * block_size_;
  // Use the next pow2 of block_size_ as alignment, so that we cover alignment
  // requests between 1 and block_size_. Users of this class should make sure
  // that requested alignment of particular blocks is never greater than the
  // block itself.
  size_t alignment = Ceil2(block_size_);
  if (alignment < block_size_) throw BadAlloc("Allocation alignment overflow");
  auto *data = reinterpret_cast<unsigned char *>(
      GetUpstreamResource()->Allocate(data_size, alignment));
  // Form a free-list of blocks in data.
  for (unsigned char i = 0U; i < blocks_per_chunk_; ++i) {
    *(data + (i * block_size_)) = i + 1U;
  }
  try {
    chunks_.push_back(Chunk{data, 0, blocks_per_chunk_});
  } catch (...) {
    GetUpstreamResource()->Deallocate(data, data_size, alignment);
    throw;
  }
  last_alloc_chunk_ = &chunks_.back();
  last_dealloc_chunk_ = &chunks_.back();
  return allocate_block_from_chunk(last_alloc_chunk_);
}

void Pool::Deallocate(void *p) {
  CHECK(last_dealloc_chunk_);
  CHECK(!chunks_.empty()) << "Expected a call to Deallocate after at least a "
                             "single Allocate has been done.";
  auto is_in_chunk = [this, p](const Chunk &chunk) {
    auto ptr = reinterpret_cast<uintptr_t>(p);
    size_t data_size = blocks_per_chunk_ * block_size_;
    return reinterpret_cast<uintptr_t>(chunk.data) <= ptr &&
           ptr < reinterpret_cast<uintptr_t>(chunk.data + data_size);
  };
  auto deallocate_block_from_chunk = [this, p](Chunk *chunk) {
    // Link the block into the free-list
    auto *block = reinterpret_cast<unsigned char *>(p);
    *block = chunk->first_available_block_ix;
    chunk->first_available_block_ix = (block - chunk->data) / block_size_;
  };
  if (is_in_chunk(*last_dealloc_chunk_)) {
    deallocate_block_from_chunk(last_dealloc_chunk_);
    return;
  }
  // Find the chunk which served this allocation
  for (auto &chunk : chunks_) {
    if (is_in_chunk(chunk)) {
      // Update last_alloc_chunk_ as well because it now has a free block.
      // Additionally this corresponds with C++ pattern of allocations and
      // deallocations being done in reverse order.
      last_alloc_chunk_ = &chunk;
      last_dealloc_chunk_ = &chunk;
      deallocate_block_from_chunk(&chunk);
      return;
    }
  }
  // TODO: We could release the Chunk to upstream memory
}

void Pool::Release() {
  for (auto &chunk : chunks_) {
    size_t data_size = blocks_per_chunk_ * block_size_;
    size_t alignment = Ceil2(block_size_);
    GetUpstreamResource()->Deallocate(chunk.data, data_size, alignment);
  }
  chunks_.clear();
}

}  // namespace impl

PoolResource::PoolResource(size_t max_blocks_per_chunk, size_t max_block_size,
                           MemoryResource *memory)
    : pools_(memory),
      unpooled_(memory),
      max_blocks_per_chunk_(
          std::min(max_blocks_per_chunk,
                   static_cast<size_t>(impl::Pool::MaxBlocksInChunk()))),
      max_block_size_(max_block_size) {
  CHECK(max_blocks_per_chunk_ > 0U);
  CHECK(max_block_size_ > 0U);
}

void *PoolResource::DoAllocate(size_t bytes, size_t alignment) {
  // Take the max of `bytes` and `alignment` so that we simplify handling
  // alignment requests.
  size_t block_size = std::max(bytes, alignment);
  // Check that we have received a regular allocation request with non-padded
  // structs/classes in play. These will always have
  // `sizeof(T) % alignof(T) == 0`. Special requests which don't have that
  // property can never be correctly handled with contiguous blocks. We would
  // have to write a general-purpose allocator which has to behave as complex
  // as malloc/free.
  if (block_size % alignment != 0)
    throw BadAlloc("Requested bytes must be a multiple of alignment");
  if (block_size > max_block_size_) {
    // Allocate a big block.
    BigBlock big_block{bytes, alignment,
                       GetUpstreamResource()->Allocate(bytes, alignment)};
    // Insert the big block in the sorted position.
    auto it = std::lower_bound(
        unpooled_.begin(), unpooled_.end(), big_block,
        [](const auto &a, const auto &b) { return a.data < b.data; });
    try {
      unpooled_.insert(it, big_block);
    } catch (...) {
      GetUpstreamResource()->Deallocate(big_block.data, bytes, alignment);
      throw;
    }
    return big_block.data;
  }
  // Allocate a regular block, first check if last_alloc_pool_ is suitable.
  if (last_alloc_pool_ && last_alloc_pool_->GetBlockSize() == block_size) {
    return last_alloc_pool_->Allocate();
  }
  // Find the pool with greater or equal block_size.
  impl::Pool pool(block_size, max_blocks_per_chunk_, GetUpstreamResource());
  auto it = std::lower_bound(pools_.begin(), pools_.end(), pool,
                             [](const auto &a, const auto &b) {
                               return a.GetBlockSize() < b.GetBlockSize();
                             });
  if (it != pools_.end() && it->GetBlockSize() == block_size) {
    last_alloc_pool_ = &*it;
    return it->Allocate();
  }
  // We don't have a pool for this block_size, so insert it in the sorted
  // position.
  it = pools_.emplace(it, std::move(pool));
  last_alloc_pool_ = &*it;
  last_dealloc_pool_ = &*it;
  return it->Allocate();
}

void PoolResource::DoDeallocate(void *p, size_t bytes, size_t alignment) {
  size_t block_size = std::max(bytes, alignment);
  CHECK(block_size % alignment == 0)
      << "PoolResource shouldn't serve allocation requests where bytes aren't "
         "a multiple of alignment";
  if (block_size > max_block_size_) {
    // Deallocate a big block.
    BigBlock big_block{bytes, alignment, p};
    auto it = std::lower_bound(
        unpooled_.begin(), unpooled_.end(), big_block,
        [](const auto &a, const auto &b) { return a.data < b.data; });
    CHECK(it != unpooled_.end());
    CHECK(it->data == p && it->bytes == bytes && it->alignment == alignment);
    unpooled_.erase(it);
    GetUpstreamResource()->Deallocate(p, bytes, alignment);
    return;
  }
  // Deallocate a regular block, first check if last_dealloc_pool_ is suitable.
  CHECK(last_dealloc_pool_);
  if (last_dealloc_pool_->GetBlockSize() == block_size)
    return last_dealloc_pool_->Deallocate(p);
  // Find the pool with equal block_size.
  impl::Pool pool(block_size, max_blocks_per_chunk_, GetUpstreamResource());
  auto it = std::lower_bound(pools_.begin(), pools_.end(), pool,
                             [](const auto &a, const auto &b) {
                               return a.GetBlockSize() < b.GetBlockSize();
                             });
  CHECK(it != pools_.end());
  CHECK(it->GetBlockSize() == block_size);
  last_alloc_pool_ = &*it;
  last_dealloc_pool_ = &*it;
  return it->Deallocate(p);
}

void PoolResource::Release() {
  for (auto &pool : pools_) pool.Release();
  pools_.clear();
  for (auto &big_block : unpooled_)
    GetUpstreamResource()->Deallocate(big_block.data, big_block.bytes,
                                      big_block.alignment);
  unpooled_.clear();
}

// PoolResource END

}  // namespace utils
