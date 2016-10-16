/**
 * Copyright (c) 2016 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "blockstore.h"
#include "log_iface.h"
#include "util.h"
#include "status_util.h"
#include "crc32c.h"

#include <cassert>

namespace Akumuli {
namespace StorageEngine {

static u64 hash32(u32 value, u32 bits, u64 seed) {
    // hashes x strongly universally into N bits
    // using the random seed.
    static const u64 a = (1ul << 32) - 1;
    return (a * value + seed) >> (64-bits);
}

static u64 hash(u64 value, u32 bits) {
    auto a = hash32(value & 0xFFFFFFFF, bits, 277);
    auto b = hash32(value >> 32, bits, 337);
    return a ^ b;
}

BlockCache::BlockCache(u32 Nbits)
    : block_cache_(1 << Nbits, PBlock())
    , bits_(Nbits)
    , gen_(dev_())
    , dist_(0, 1 << Nbits)
{
}

int BlockCache::probe(LogicAddr addr) {
    auto h = hash(addr, bits_);
    auto b = block_cache_.at(h);
    if (b) {
        return b->get_addr() == addr ? 2 : 1;
    }
    return 0;
}

void BlockCache::insert(PBlock block) {
    auto addr = block->get_addr();
    auto pr = probe(addr);
    if (pr == 2) {
        // No need to insert, addr already sits in the cache.
        return;
    }
    if (pr == 0) {
        // Eviction. Generate two random hashes. Evict least accessed.
        auto h1 = dist_(gen_);
        auto h2 = dist_(gen_);
        auto p1 = block_cache_.at(h1);
        auto p2 = block_cache_.at(h2);
        if (p1 && p2) {
            if (p1.use_count() > p2.use_count()) {
                block_cache_.at(h2).reset();
            } else if (p1.use_count() < p2.use_count()) {
                block_cache_.at(h1).reset();
            } else {
                if (p1->get_addr() < p2->get_addr()) {
                    block_cache_.at(h1).reset();
                } else {
                    block_cache_.at(h2).reset();
                }
            }
        }
    }
    auto h = hash(addr, bits_);
    block_cache_.at(h) = block;
}

BlockCache::PBlock BlockCache::loockup(LogicAddr addr) {
    auto it = hash(addr, bits_);
    auto p = block_cache_.at(it);
    if (p->get_addr() != addr) {
        p.reset();
    }
    return p;
}


Block::Block(LogicAddr addr, std::vector<u8>&& data)
    : data_(std::move(data))
    , addr_(addr)
{
}

Block::Block()
    : data_(static_cast<size_t>(AKU_BLOCK_SIZE), 0)
    , addr_(EMPTY_ADDR)
{
}

const u8* Block::get_data() const {
    return data_.data();
}

u8* Block::get_data() {
    return data_.data();
}

size_t Block::get_size() const {
    return data_.size();
}

LogicAddr Block::get_addr() const {
    return addr_;
}

void Block::set_addr(LogicAddr addr) {
    addr_ = addr;
}

FixedSizeFileStorage::FixedSizeFileStorage(std::string metapath, std::vector<std::string> volpaths)
    : meta_(MetaVolume::open_existing(metapath.c_str()))
    , current_volume_(0)
    , current_gen_(0)
    , total_size_(0)
    , volume_names_(volpaths)
{
    for (u32 ix = 0ul; ix < volpaths.size(); ix++) {
        auto volpath = volpaths.at(ix);
        u32 nblocks = 0;
        aku_Status status = AKU_SUCCESS;
        std::tie(status, nblocks) = meta_->get_nblocks(ix);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, std::string("Can't open blockstore, volume " +
                                                   std::to_string(ix) + " failure: " +
                                                   StatusUtil::str(status)));
            AKU_PANIC("Can't open blockstore - " + StatusUtil::str(status));
        }
        auto uptr = Volume::open_existing(volpath.c_str(), nblocks);
        volumes_.push_back(std::move(uptr));
        dirty_.push_back(0);
    }

    for (const auto& vol: volumes_) {
        total_size_ += vol->get_size();
    }

    // set current volume, current volume is a first volume with free space available
    for (u32 i = 0u; i < volumes_.size(); i++) {
        u32 curr_gen, nblocks;
        aku_Status status;
        std::tie(status, curr_gen) = meta_->get_generation(i);
        if (status == AKU_SUCCESS) {
            std::tie(status, nblocks) = meta_->get_nblocks(i);
        } else {
            Logger::msg(AKU_LOG_ERROR, "Can't find current volume, meta-volume corrupted, error: "
                        + StatusUtil::str(status));
            AKU_PANIC("Meta-volume corrupted, " + StatusUtil::str(status));
        }
        if (volumes_[i]->get_size() > nblocks) {
            // Free space available
            current_volume_ = i;
            current_gen_ = curr_gen;
            break;
        }
    }
}

std::shared_ptr<FixedSizeFileStorage> FixedSizeFileStorage::open(std::string metapath, std::vector<std::string> volpaths) {
    auto bs = new FixedSizeFileStorage(metapath, volpaths);
    return std::shared_ptr<FixedSizeFileStorage>(bs);
}

void FixedSizeFileStorage::create(std::string metapath,
                                  std::vector<std::tuple<u32, std::string>> vols)
{
    std::vector<u32> caps;
    for (auto cp: vols) {
        std::string path;
        u32 capacity;
        std::tie(capacity, path) = cp;
        Volume::create_new(path.c_str(), capacity);
        caps.push_back(capacity);
    }
    MetaVolume::create_new(metapath.c_str(), caps.size(), caps.data());
}

static u32 extract_gen(LogicAddr addr) {
    return addr >> 32;
}

static BlockAddr extract_vol(LogicAddr addr) {
    return addr & 0xFFFFFFFF;
}

static LogicAddr make_logic(u32 gen, BlockAddr addr) {
    return static_cast<u64>(gen) << 32 | addr;
}

bool FixedSizeFileStorage::exists(LogicAddr addr) const {
    std::lock_guard<std::mutex> guard(lock_); AKU_UNUSED(guard);
    auto gen = extract_gen(addr);
    auto vol = extract_vol(addr);
    auto volix = gen % static_cast<u32>(volumes_.size());
    aku_Status status;
    u32 actual_gen;
    std::tie(status, actual_gen) = meta_->get_generation(volix);
    if (status != AKU_SUCCESS) {
        return false;
    }
    u32 nblocks;
    std::tie(status, nblocks) = meta_->get_nblocks(volix);
    if (status != AKU_SUCCESS) {
        return false;
    }
    return actual_gen == gen && vol < nblocks;
}

std::tuple<aku_Status, std::shared_ptr<Block>> FixedSizeFileStorage::read_block(LogicAddr addr) {
    std::lock_guard<std::mutex> guard(lock_); AKU_UNUSED(guard);
    aku_Status status;
    auto gen = extract_gen(addr);
    auto vol = extract_vol(addr);
    auto volix = gen % static_cast<u32>(volumes_.size());
    u32 actual_gen;
    u32 nblocks;
    std::tie(status, actual_gen) = meta_->get_generation(volix);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(AKU_EBAD_ARG, std::unique_ptr<Block>());
    }
    std::tie(status, nblocks) = meta_->get_nblocks(volix);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(AKU_EBAD_ARG, std::unique_ptr<Block>());
    }
    if (actual_gen != gen || vol >= nblocks) {
        return std::make_tuple(AKU_EBAD_ARG, std::unique_ptr<Block>());
    }
    std::vector<u8> dest(AKU_BLOCK_SIZE, 0);
    status = volumes_[volix]->read_block(vol, dest.data());
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, std::unique_ptr<Block>());
    }
    auto block = std::make_shared<Block>(addr, std::move(dest));
    return std::make_tuple(status, std::move(block));
}

void FixedSizeFileStorage::advance_volume() {
    Logger::msg(AKU_LOG_INFO, "Advance volume called, current gen:" + std::to_string(current_gen_));
    current_volume_ = (current_volume_ + 1) % volumes_.size();
    aku_Status status;
    std::tie(status, current_gen_) = meta_->get_generation(current_volume_);
    if (status != AKU_SUCCESS) {
        Logger::msg(AKU_LOG_ERROR, "Can't read generation of next volume, " + StatusUtil::str(status));
        AKU_PANIC("Can't read generation of the next volume, " + StatusUtil::str(status));
    }
    // If volume is not empty - reset it and change generation
    u32 nblocks;
    std::tie(status, nblocks) = meta_->get_nblocks(current_volume_);
    if (status != AKU_SUCCESS) {
        Logger::msg(AKU_LOG_ERROR, "Can't read nblocks of next volume, " + StatusUtil::str(status));
        AKU_PANIC("Can't read nblocks of the next volume, " + StatusUtil::str(status));
    }
    if (nblocks != 0) {
        current_gen_ += volumes_.size();
        auto status = meta_->set_generation(current_volume_, current_gen_);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Can't set generation on volume, " + StatusUtil::str(status));
            AKU_PANIC("Invalid BlockStore state, can't reset volume's generation, " + StatusUtil::str(status));
        }
        // Rest selected volume
        status = meta_->set_nblocks(current_volume_, 0);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Can't reset nblocks on volume, " + StatusUtil::str(status));
            AKU_PANIC("Invalid BlockStore state, can't reset volume's nblocks, " + StatusUtil::str(status));
        }
        volumes_[current_volume_]->reset();
        dirty_[current_volume_]++;
    }
}

std::tuple<aku_Status, LogicAddr> FixedSizeFileStorage::append_block(std::shared_ptr<Block> data) {
    std::lock_guard<std::mutex> guard(lock_); AKU_UNUSED(guard);
    BlockAddr block_addr;
    aku_Status status;
    std::tie(status, block_addr) = volumes_[current_volume_]->append_block(data->get_data());
    if (status == AKU_EOVERFLOW) {
        // Move to next generation
        advance_volume();
        std::tie(status, block_addr) = volumes_.at(current_volume_)->append_block(data->get_data());
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, 0ull);
        }
    }
    data->set_addr(block_addr);
    status = meta_->set_nblocks(current_volume_, block_addr + 1);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("Invalid BlockStore state, " + StatusUtil::str(status));
    }
    dirty_[current_volume_]++;
    return std::make_tuple(status, make_logic(current_gen_, block_addr));
}

void FixedSizeFileStorage::flush() {
    std::lock_guard<std::mutex> guard(lock_); AKU_UNUSED(guard);
    /*
    for (size_t ix = 0; ix < dirty_.size(); ix++) {
        if (dirty_[ix]) {
            dirty_[ix] = 0;
            volumes_[ix]->flush();
        }
    }
    */
    for (size_t ix = 0; ix < volumes_.size(); ix++) {
        volumes_[ix]->flush();
    }
    meta_->flush();
}

BlockStoreStats FixedSizeFileStorage::get_stats() const {
    BlockStoreStats stats = {};
    stats.block_size = 4096;
    size_t nvol = meta_->get_nvolumes();
    for (u32 ix = 0; ix < nvol; ix++) {
        aku_Status stat;
        u32 res;
        std::tie(stat, res) = meta_->get_capacity(ix);
        if (stat == AKU_SUCCESS) {
            stats.capacity += res;
        }
        std::tie(stat, res) = meta_->get_nblocks(ix);
        if (stat == AKU_SUCCESS) {
            stats.nblocks += res;
        }
    }
    return stats;
}

PerVolumeStats FixedSizeFileStorage::get_volume_stats() const {
    PerVolumeStats result;
    size_t nvol = meta_->get_nvolumes();
    for (u32 ix = 0; ix < nvol; ix++) {
        BlockStoreStats stats = {};
        stats.block_size = 4096;
        aku_Status stat;
        u32 res;
        std::tie(stat, res) = meta_->get_capacity(ix);
        if (stat == AKU_SUCCESS) {
            stats.capacity += res;
        }
        std::tie(stat, res) = meta_->get_nblocks(ix);
        if (stat == AKU_SUCCESS) {
            stats.nblocks += res;
        }
        auto name = volume_names_.at(ix);
        result[name] = stats;
    }
    return result;

}

static u32 crc32c(const u8* data, size_t size) {
    static crc32c_impl_t impl = chose_crc32c_implementation();
    return impl(0, data, size);
}

u32 FixedSizeFileStorage::checksum(u8 const* data, size_t size) const {
    return crc32c(data, size);
}

//! Address space should be started from this address (otherwise some tests will pass no matter what).
static const LogicAddr MEMSTORE_BASE = 619;

// MemStore
MemStore::MemStore()
    : write_pos_(0)
    , removed_pos_(0)
{
}

MemStore::MemStore(std::function<void(LogicAddr)> append_cb)
    : append_callback_(append_cb)
    , write_pos_(0)
    , removed_pos_(0)
{
}

void MemStore::remove(size_t addr) {
    removed_pos_ = addr;
}

u32 MemStore::checksum(u8 const* data, size_t size) const {
    return crc32c(data, size);
}

std::tuple<aku_Status, std::shared_ptr<Block>> MemStore::read_block(LogicAddr addr) {
    addr -= MEMSTORE_BASE;
    std::lock_guard<std::mutex> guard(lock_); AKU_UNUSED(guard);
    std::shared_ptr<Block> block;
    u32 offset = static_cast<u32>(AKU_BLOCK_SIZE * addr);
    if (buffer_.size() < (offset + AKU_BLOCK_SIZE)) {
        return std::make_tuple(AKU_EBAD_ARG, block);
    }
    if (addr < removed_pos_) {
        return std::make_tuple(AKU_EBAD_ARG, block);
    }
    std::vector<u8> data;
    data.reserve(AKU_BLOCK_SIZE);
    auto begin = buffer_.begin() + offset;
    auto end = begin + AKU_BLOCK_SIZE;
    std::copy(begin, end, std::back_inserter(data));
    block.reset(new Block(addr + MEMSTORE_BASE, std::move(data)));
    return std::make_tuple(AKU_SUCCESS, block);
}

std::tuple<aku_Status, LogicAddr> MemStore::append_block(std::shared_ptr<Block> data) {
    std::lock_guard<std::mutex> guard(lock_); AKU_UNUSED(guard);
    assert(data->get_size() == AKU_BLOCK_SIZE);
    std::copy(data->get_data(), data->get_data() + AKU_BLOCK_SIZE, std::back_inserter(buffer_));
    if (append_callback_) {
        append_callback_(write_pos_ + MEMSTORE_BASE);
    }
    auto addr = write_pos_++;
    addr += MEMSTORE_BASE;
    data->set_addr(addr);
    return std::make_tuple(AKU_SUCCESS, addr);
}

void MemStore::flush() {
    // no-op
}

BlockStoreStats MemStore::get_stats() const {
    BlockStoreStats s;
    s.block_size = 4096;
    s.capacity = 1024*4096;
    s.nblocks = write_pos_;
    return s;
}

PerVolumeStats MemStore::get_volume_stats() const {
    PerVolumeStats result;
    BlockStoreStats s;
    s.block_size = 4096;
    s.capacity = 1024*4096;
    s.nblocks = write_pos_;
    result["mem"] = s;
    return result;
}

bool MemStore::exists(LogicAddr addr) const {
    addr -= MEMSTORE_BASE;
    std::lock_guard<std::mutex> guard(lock_); AKU_UNUSED(guard);
    return addr < write_pos_;
}

std::shared_ptr<BlockStore> BlockStoreBuilder::create_memstore() {
    return std::make_shared<MemStore>();
}

std::shared_ptr<BlockStore> BlockStoreBuilder::create_memstore(std::function<void(LogicAddr)> append_cb) {
    return std::make_shared<MemStore>(append_cb);
}

}}  // namespace
