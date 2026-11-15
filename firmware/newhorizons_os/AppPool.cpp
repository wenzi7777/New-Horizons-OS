#include "AppPool.h"

#include <string.h>

namespace nhos {

uint32_t AppPoolCore::boundary() const {
  for (uint16_t i = 0; i < count_; ++i) {
    if (blocks_[i].fixed()) return blocks_[i].at;
  }
  return bytes_;
}

uint32_t AppPoolCore::movableEnd() const {
  uint32_t end = 0;
  for (uint16_t i = 0; i < count_; ++i) {
    if (!blocks_[i].fixed() && blocks_[i].at + blocks_[i].len > end) {
      end = blocks_[i].at + blocks_[i].len;
    }
  }
  return end;
}

uint32_t AppPoolCore::movableUsed() const {
  uint32_t used = 0;
  for (uint16_t i = 0; i < count_; ++i) {
    if (!blocks_[i].fixed()) used += blocks_[i].len;
  }
  return used;
}

uint32_t AppPoolCore::usedBytes() const {
  uint32_t used = 0;
  for (uint16_t i = 0; i < count_; ++i) used += blocks_[i].len;
  return used;
}

bool AppPoolCore::owns(const void* memory) const {
  for (uint16_t i = 0; i < count_; ++i) {
    if (memory_ + blocks_[i].at == memory) return true;
  }
  return false;
}

bool AppPoolCore::insert(const Block& block) {
  if (count_ >= maxBlocks_) return false;
  uint16_t at = count_;
  while (at > 0 && blocks_[at - 1].at > block.at) {
    blocks_[at] = blocks_[at - 1];
    --at;
  }
  blocks_[at] = block;
  ++count_;
  return true;
}

void AppPoolCore::compact() {
  uint32_t cursor = 0;
  for (uint16_t i = 0; i < count_; ++i) {
    Block& block = blocks_[i];
    if (block.fixed()) continue;
    if (block.at != cursor) {
      memmove(memory_ + cursor, memory_ + block.at, block.len);
      block.at = static_cast<uint16_t>(cursor);
      if (relocate_ != nullptr) relocate_(block.owner, memory_ + cursor);
    }
    cursor += block.len;
  }
}

void* AppPoolCore::allocateFixed(uint32_t len) {
  len = align4(len);
  if (len == 0 || len > bytes_ || count_ >= maxBlocks_) return nullptr;
  // A hole among the fixed blocks first, highest first: an app removed from
  // the middle leaves one exactly the size of the next app's control block.
  uint32_t holeEnd = bytes_;
  for (int32_t i = static_cast<int32_t>(count_) - 1; i >= 0; --i) {
    const Block& block = blocks_[i];
    if (!block.fixed()) break;  // every movable block is below every fixed one
    const uint32_t blockEnd = block.at + block.len;
    if (holeEnd - blockEnd >= len) {
      Block made;
      made.at = static_cast<uint16_t>(holeEnd - len);
      made.len = static_cast<uint16_t>(len);
      insert(made);
      return memory_ + made.at;
    }
    holeEnd = block.at;
  }
  // Otherwise below the fixed region, where the movables may have to make room.
  const uint32_t limit = boundary();
  if (limit < len || movableUsed() > limit - len) return nullptr;
  if (movableEnd() > limit - len) compact();
  Block made;
  made.at = static_cast<uint16_t>(limit - len);
  made.len = static_cast<uint16_t>(len);
  if (!insert(made)) return nullptr;
  return memory_ + made.at;
}

uint8_t* AppPoolCore::allocateMovable(void* owner, uint32_t len) {
  len = align4(len);
  if (owner == nullptr || len == 0 || len > bytes_ || count_ >= maxBlocks_) return nullptr;
  const uint32_t limit = boundary();
  // First fit between the movable blocks.
  uint32_t cursor = 0;
  bool found = false;
  for (uint16_t i = 0; i <= count_; ++i) {
    const bool last = i == count_ || blocks_[i].fixed();
    const uint32_t end = last ? limit : blocks_[i].at;
    if (end >= cursor && end - cursor >= len) {
      found = true;
      break;
    }
    if (last) break;
    cursor = blocks_[i].at + blocks_[i].len;
  }
  if (!found) {
    if (limit < movableUsed() || limit - movableUsed() < len) return nullptr;
    compact();
    cursor = movableUsed();
  }
  Block made;
  made.at = static_cast<uint16_t>(cursor);
  made.len = static_cast<uint16_t>(len);
  made.owner = owner;
  if (!insert(made)) return nullptr;
  return memory_ + cursor;
}

void AppPoolCore::release(const void* memory) {
  for (uint16_t i = 0; i < count_; ++i) {
    if (memory_ + blocks_[i].at != memory) continue;
    for (uint16_t j = i; j + 1 < count_; ++j) blocks_[j] = blocks_[j + 1];
    --count_;
    return;
  }
}

}  // namespace nhos
