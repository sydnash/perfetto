/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/memory/page_idle_checker.h"
#include "src/profiling/memory/utils.h"

#include <inttypes.h>
#include <vector>

namespace perfetto {
namespace profiling {
namespace {

constexpr uint64_t kIsInRam = 1ULL << 63;
constexpr uint64_t kRamPhysicalPageMask = ~(~0ULL << 55);

}  // namespace

int64_t PageIdleChecker::OnIdlePage(uint64_t addr, size_t size) {
  uint64_t page_nr = addr / base::kPageSize;
  uint64_t end_page_nr = (addr + size) / base::kPageSize;
  // The trailing division will have rounded down, unless the end is at a page
  // boundary. Add one page if we rounded down.
  if ((addr + size) % base::kPageSize != 0)
    end_page_nr++;

  size_t pages = end_page_nr - page_nr;
  std::vector<uint64_t> virt_page_infos(pages);

  off64_t virt_off = static_cast<off64_t>(page_nr * sizeof(virt_page_infos[0]));
  size_t virt_rd_size = pages * sizeof(virt_page_infos[0]);
  ssize_t rd = ReadAtOffsetClobberSeekPos(*pagemap_fd_, &(virt_page_infos[0]),
                                          virt_rd_size, virt_off);
  if (rd != static_cast<ssize_t>(virt_rd_size)) {
    PERFETTO_ELOG("Invalid read from pagemap: %zd", rd);
    return -1;
  }

  int64_t idle_mem = 0;

  for (size_t i = 0; i < pages; ++i) {
    if (!virt_page_infos[i]) {
      PERFETTO_DLOG("Empty pageinfo.");
      continue;
    }

    if (!(virt_page_infos[i] & kIsInRam)) {
      PERFETTO_DLOG("Page is not in RAM.");
      continue;
    }

    uint64_t phys_page_nr = virt_page_infos[i] & kRamPhysicalPageMask;
    if (!phys_page_nr) {
      PERFETTO_ELOG("Failed to get physical page number.");
      continue;
    }

    int idle = IsPageIdle(phys_page_nr);
    if (idle == -1)
      continue;

    if (idle) {
      if (i == 0)
        idle_mem += GetFirstPageShare(addr, size);
      else if (i == pages - 1)
        idle_mem += GetLastPageShare(addr, size);
      else
        idle_mem += base::kPageSize;
    } else {
      touched_phys_page_nrs_.emplace(phys_page_nr);
    }
  }
  return idle_mem;
}

void PageIdleChecker::MarkPagesIdle() {
  for (uint64_t phys_page_nr : touched_phys_page_nrs_)
    MarkPageIdle(phys_page_nr);
  touched_phys_page_nrs_.clear();
}

void PageIdleChecker::MarkPageIdle(uint64_t phys_page_nr) {
  // The file implements a bitmap where each bit corresponds to a memory page.
  // The bitmap is represented by an array of 8-byte integers, and the page at
  // PFN #i is mapped to bit #i%64 of array element #i/64, byte order i
  // native. When a bit is set, the corresponding page is idle.
  //
  // The kernel ORs the value written with the existing bitmap, so we do not
  // override previously written values.
  // See https://www.kernel.org/doc/Documentation/vm/idle_page_tracking.txt
  off64_t offset = 8 * (phys_page_nr / 64);
  size_t bit_offset = phys_page_nr % 64;
  uint64_t bit_pattern = 1 << bit_offset;
  if (WriteAtOffsetClobberSeekPos(*bitmap_fd_, &bit_pattern,
                                  sizeof(bit_pattern), offset) !=
      static_cast<ssize_t>(sizeof(bit_pattern))) {
    PERFETTO_PLOG("Failed to write bit pattern at %" PRIi64 ".", offset);
  }
}

int PageIdleChecker::IsPageIdle(uint64_t phys_page_nr) {
  off64_t offset = 8 * (phys_page_nr / 64);
  size_t bit_offset = phys_page_nr % 64;
  uint64_t bit_pattern;
  if (ReadAtOffsetClobberSeekPos(*bitmap_fd_, &bit_pattern, sizeof(bit_pattern),
                                 offset) !=
      static_cast<ssize_t>(sizeof(bit_pattern))) {
    PERFETTO_PLOG("Failed to read bit pattern at %" PRIi64 ".", offset);
    return -1;
  }
  return static_cast<int>(bit_pattern & (1 << bit_offset));
}

uint64_t GetFirstPageShare(uint64_t addr, size_t size) {
  // Our allocation is xxxx in this illustration:
  //         +----------------------------------------------+
  //         |             xxxxxxxxxx|xxxxxx                |
  //         |             xxxxxxxxxx|xxxxxx                |
  //         |             xxxxxxxxxx|xxxxxx                |
  //         +-------------+---------------+----------------+
  //         ^             ^         ^     ^
  //         +             +         +     +
  // page_aligned_addr  addr        end    addr + size
  uint64_t page_aligned_addr = (addr / base::kPageSize) * base::kPageSize;
  uint64_t end = page_aligned_addr + base::kPageSize;
  if (end > addr + size) {
    // The whole allocation is on the first page.
    return size;
  }

  return base::kPageSize - (addr - page_aligned_addr);
}

uint64_t GetLastPageShare(uint64_t addr, size_t size) {
  uint64_t last_page_size = (addr + size) % base::kPageSize;
  if (last_page_size == 0) {
    // Address ends at a page boundary, the whole last page is idle.
    return base::kPageSize;
  } else {
    // Address does not end at a page boundary, only a subset of the last
    // page should be attributed to this allocation.
    return last_page_size;
  }
}

}  // namespace profiling
}  // namespace perfetto
