/**
 * PRIVATE HEADER
 *
 * Compression algorithms
 *
 * Copyright (c) 2013 Eugene Lazin <4lazin@gmail.com>
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


#pragma once

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <vector>
#include <stdexcept>

#include "akumuli.h"

namespace Akumuli {

struct StreamOutOfBounds : std::runtime_error {
    StreamOutOfBounds(const char* msg);
};

typedef std::vector<unsigned char> ByteVector;

struct UncompressedChunk {
    /** Index in `timestamps` and `paramids` arrays corresponds
      * to individual row. Each element of the `values` array corresponds to
      * specific column and row. Variable longest_row should contain
      * longest row length inside the header.
      */
    std::vector<aku_Timestamp>  timestamps;
    std::vector<aku_ParamId>    paramids;
    std::vector<double>         values;
};

struct ChunkWriter {

    virtual ~ChunkWriter() = default;

    /** Allocate space for new data. Return mem range or
      * empty range in a case of error.
      */
    virtual aku_MemRange allocate() = 0;

    //! Commit changes
    virtual aku_Status commit(size_t bytes_written) = 0;
};

//! Base 128 encoded integer
template<class TVal>
class Base128Int {
    TVal value_;
    typedef unsigned char byte_t;
    typedef byte_t* byte_ptr;
public:

    Base128Int(TVal val) : value_(val) {
    }

    Base128Int() : value_() {
    }

    /** Read base 128 encoded integer from the binary stream
      * FwdIter - forward iterator.
      */
    const unsigned char* get(const unsigned char* begin, const unsigned char* end) {
        assert(begin < end);

        auto acc = TVal();
        auto cnt = TVal();
        const unsigned char* p = begin;

        while (true) {
            if (p == end) {
                return begin;
            }
            auto i = static_cast<byte_t>(*p & 0x7F);
            acc |= TVal(i) << cnt;
            if ((*p++ & 0x80) == 0) {
                break;
            }
            cnt += 7;
        }
        value_ = acc;
        return p;
    }

    /** Write base 128 encoded integer to the binary stream.
      * @returns 'begin' on error, iterator to next free region otherwise
      */
    unsigned char* put(unsigned char* begin, const unsigned char* end) const {
        if (begin >= end) {
            return begin;
        }

        TVal value = value_;
        unsigned char* p = begin;

        while (true) {
            if (p == end) {
                return begin;
            }
            *p = value & 0x7F;
            value >>= 7;
            if (value != 0) {
                *p++ |= 0x80;
            } else {
                p++;
                break;
            }
        }
        return p;
    }

    //! turn into integer
    operator TVal() const {
        return value_;
    }
};

//! Base128 encoder
struct Base128StreamWriter {
    // underlying memory region
    const unsigned char *begin_;
    const unsigned char *end_;
    unsigned char       *pos_;

    Base128StreamWriter(unsigned char* begin, const unsigned char* end)
        : begin_(begin)
        , end_(end)
        , pos_(begin)
    {
    }

    Base128StreamWriter(Base128StreamWriter& other)
        : begin_(other.begin_)
        , end_(other.end_)
        , pos_(other.pos_)
    {
    }

    /** Put value into stream.
     */
    template<class TVal>
    void put(TVal value) {
        Base128Int<TVal> val(value);
        unsigned char* p = val.put(pos_, end_);
        if (pos_ == p) {
            throw StreamOutOfBounds("can't write value, out of bounds");
        }
        pos_ = p;
    }

    void put_raw(unsigned char value) {
        if (pos_ == end_) {
            throw StreamOutOfBounds("can't write value, out of bounds");
        }
        *pos_++ = value;
    }

    void put_raw(uint32_t value) {
        if ((end_ - pos_) < (int)sizeof(value)) {
            throw StreamOutOfBounds("can't write value, out of bounds");
        }
        *reinterpret_cast<uint32_t*>(pos_) = value;
        pos_ += sizeof(value);
    }

    void put_raw(uint64_t value) {
        if ((end_ - pos_) < (int)sizeof(value)) {
            throw StreamOutOfBounds("can't write value, out of bounds");
        }
        *reinterpret_cast<uint64_t*>(pos_) = value;
        pos_ += sizeof(value);
    }


    //! Commit stream
    void commit() {}

    size_t size() const {
        return pos_ - begin_;
    }

    size_t space_left() const {
        return end_ - pos_;
    }

    uint8_t* pos() {
        return pos_;
    }

    /** Try to allocate space inside a stream in current position without
      * compression (needed for size prefixes).
      * @returns pointer to the value inside the stream
      * @throw StreamOutOfBounds if there is not enough space for value
      */
    template<class T>
    T* allocate() {
        size_t sz = sizeof(T);
        if (space_left() < sz) {
            throw StreamOutOfBounds("can't allocate value, not enough space");
        }
        T* result = reinterpret_cast<T*>(pos_);
        pos_ += sz;
        return result;
    }
};

//! Base128 decoder
struct Base128StreamReader {
    const unsigned char* pos_;
    const unsigned char* end_;

    Base128StreamReader(const unsigned char* begin, const unsigned char* end)
        : pos_(begin)
        , end_(end)
    {
    }

    template<class TVal>
    TVal next() {
        Base128Int<TVal> value;
        auto p = value.get(pos_, end_);
        if (p == pos_) {
            throw StreamOutOfBounds("can't read value, out of bounds");
        }
        pos_ = p;
        return static_cast<TVal>(value);
    }

    //! Read uncompressed value from stream
    template<class TVal>
    TVal read_raw() {
        size_t sz = sizeof(TVal);
        if (space_left() < sz) {
            throw StreamOutOfBounds("can't read value, out of bounds");
        }
        auto val = *reinterpret_cast<const TVal*>(pos_);
        pos_ += sz;
        return val;
    }

    size_t space_left() const {
        return end_ - pos_;
    }

    const unsigned char* pos() const {
        return pos_;
    }
};

/** Base128 encoder reimplementation
  * New encoding is used. Stream is divided into 8byte chunks.
  * Each 8byte chunk is precedded by 1byte control block.
  * If i-th bit in control block is set then i-th byte of the block
  * is an end of the integer value. It takes one byte to store integers in range [0 - 0xFF],
  * two bytes to store integers in range [0x100 - 0xFFFF], etc.
  * Integer values can't cross block border, so you can't save value 0xFFFFFFFF if there is only
  * three bytes left in block. This value should be stored in the next block.
  * This method internal fragmentation because sometimes blocks wouldn't be filled entirely.
  */
struct Base128StreamWriterV2 {
    unsigned char* outbuf_;  // output buffer
    size_t size_;            // size of the output buffer
    size_t pos_;             // position in output buffer
    unsigned char ctrl_;     // control byte
    size_t ctrl_index_;      // control byte index in output

    enum {
        BLOCK_SIZE = 9
    };

    Base128StreamWriterV2(unsigned char* begin, const unsigned char* end)
        : outbuf_(begin)
        , size_(end - begin)
        , pos_(1)
        , ctrl_(0)
        , ctrl_index_(0)
    {
    }

    Base128StreamWriterV2(Base128StreamWriterV2& other)
        : outbuf_(other.outbuf_)
        , size_(other.size_)
        , pos_(1)
        , ctrl_(0)
        , ctrl_index_(0)
    {
    }

    int curr_block_index() const {
        return pos_ - (ctrl_index_ + 1);
    }

    int curr_block_cap() const {
        return 8 - curr_block_index();
    }

    /** Put value into stream.
     */
    template<class TVal>
    void put(TVal value) {
        int leading_zeroes = 56;  // if value is 0 we should store 1 byte
        if (value) {
            leading_zeroes = __builtin_clzl(value);
        }
        int nbytes = (64 - leading_zeroes / 8 * 8) / 8;
        if (nbytes > curr_block_cap()) {
            // move to next block
            move_to_next_block();
        }
        size_t end = pos_ + nbytes;
        for (; pos_ < end; pos_++) {
            unsigned char byte = value & 0xFF;
            outbuf_[pos_] = byte;
            value >>= 8;
        }
        ctrl_ |= (1 << (curr_block_index() - 1));
        // -1 because pos points to the next free byte of the block here
    }

    void commit() {
        outbuf_[ctrl_index_] = ctrl_;
    }

    void move_to_next_block() {
        outbuf_[ctrl_index_] = ctrl_;
        ctrl_ = 0;
        ctrl_index_ += BLOCK_SIZE;
        pos_ = ctrl_index_ + 1;
        if (ctrl_index_ > size_) {
            throw StreamOutOfBounds("out of memory");
        }
    }

    size_t size() const {
        return pos_;
    }

    size_t space_left() const {
        return size_ - pos_;
    }

    // "Raw" interface

    //! Put raw value to the output stream. `put` method wouldn't work if this method were called.
    void put_raw(unsigned char value) {
        if (pos_ == size_) {
            throw StreamOutOfBounds("can't write value, out of bounds");
        }
        outbuf_[pos_++] = value;
    }

    //! Put raw value to the output stream. `put` method wouldn't work if this method were called.
    void put_raw(uint32_t value) {
        if ((pos_ + sizeof(value)) > size_) {
            throw StreamOutOfBounds("can't write value, out of memory");
        }
        *reinterpret_cast<uint64_t*>(outbuf_ + pos_) = value;
        pos_ += sizeof(value);
    }

    //! Put raw value to the output stream. `put` method wouldn't work if this method were called.
    void put_raw(uint64_t value) {
        if (pos_ + sizeof(value) > size_) {
            throw StreamOutOfBounds("can't write value, out of memory");
        }
        *reinterpret_cast<uint64_t*>(outbuf_ + pos_) = value;
        pos_ += sizeof(value);
    }

    /** Try to allocate space inside a stream in current position without
      * compression (needed for size prefixes).
      * @returns pointer to the value inside the stream
      * @throw StreamOutOfBounds if there is not enough space for value
      */
    template<class T>
    T* allocate() {
        size_t sz = sizeof(T);
        if (space_left() < sz) {
            throw StreamOutOfBounds("can't allocate value, not enough space");
        }
        T* result = reinterpret_cast<T*>(outbuf_ + pos_);
        pos_ += sz;
        return result;
    }
};

//! Base128 decoder
struct Base128StreamReaderV2 {
    const unsigned char* input_;
    const size_t size_;
    size_t pos_;
    size_t ctrl_index_;
    unsigned char ctrl_;
    int bit_index_;

    enum {
        BLOCK_SIZE = 9
    };

    Base128StreamReaderV2(const unsigned char* begin, const unsigned char* end)
        : input_(begin)
        , size_(end - begin)
        , pos_(1)
        , ctrl_index_(0)
        , ctrl_(0)
        , bit_index_(0)
    {
        assert(size_);
        ctrl_ = input_[0];
    }

    //! Move to next 8byte block
    void _next_block() {
        ctrl_index_ += BLOCK_SIZE;
        pos_ = ctrl_index_ + 1;
        ctrl_ = input_[ctrl_index_];
        bit_index_ = 0;
    }

    template<class TVal>
    TVal next() {
        TVal result = 0;
        int shift = 0;
        int mask = 0;
        do {
            result |= static_cast<TVal>(input_[pos_]) << shift;
            pos_++;
            shift += 8;
            mask = 1 << bit_index_++;
            if (shift == 64) {
                break;
            }
        } while((ctrl_ & mask) == 0);
        if ((ctrl_ >> bit_index_) == 0) {
            // proceed to the next block if this block is completed
            _next_block();
        }
        return result;
    }

    //! Read uncompressed value from stream
    template<class TVal>
    TVal read_raw() {
        size_t sz = sizeof(TVal);
        if (space_left() < sz) {
            throw StreamOutOfBounds("can't read value, out of bounds");
        }
        auto val = *reinterpret_cast<const TVal*>(input_ + pos_);
        pos_ += sz;
        return val;
    }

    size_t space_left() const {
        return size_ - pos_;
    }

    const unsigned char* pos() const {
        return input_ + pos_;
    }
};

template<class Stream, class TVal>
struct ZigZagStreamWriter {
    Stream stream_;

    ZigZagStreamWriter(Base128StreamWriter& stream)
        : stream_(stream)
    {
    }
    void put(TVal value) {
        // TVal should be signed
        const int shift_width = sizeof(TVal)*8 - 1;
        auto res = (value << 1) ^ (value >> shift_width);
        stream_.put(res);
    }
    size_t size() const {
        return stream_.size();
    }
    void commit() {
        stream_.commit();
    }
};

template<class Stream, class TVal>
struct ZigZagStreamReader {
    Stream stream_;

    ZigZagStreamReader(Base128StreamReader& stream)
        : stream_(stream) {
    }

    TVal next() {
        auto n = stream_.next();
        return (n >> 1) ^ (-(n & 1));
    }

    unsigned char* pos() const {
        return stream_.pos();
    }
};

template<class Stream, typename TVal>
struct DeltaStreamWriter {
    Stream stream_;
    TVal prev_;

    DeltaStreamWriter(Base128StreamWriter& stream)
        : stream_(stream)
        , prev_()
    {
    }

    void put(TVal value) {
        stream_.put(static_cast<TVal>(value) - prev_);
        prev_ = value;
    }

    size_t size() const {
        return stream_.size();
    }

    void commit() {
        stream_.commit();
    }
};


template<class Stream, typename TVal>
struct DeltaStreamReader {
    Stream stream_;
    TVal prev_;

    DeltaStreamReader(Base128StreamReader& stream)
        : stream_(stream)
        , prev_()
    {
    }

    TVal next() {
        TVal delta = stream_.next();
        TVal value = prev_ + delta;
        prev_ = value;
        return value;
    }

    unsigned char* pos() const {
        return stream_.pos();
    }
};


template<typename TVal>
struct RLEStreamWriter {
    Base128StreamWriter& stream_;
    TVal prev_;
    TVal reps_;
    size_t start_size_;

    RLEStreamWriter(Base128StreamWriter& stream)
        : stream_(stream)
        , prev_()
        , reps_()
        , start_size_(stream.size())
    {}

    void put(TVal value) {
        if (value != prev_) {
            if (reps_) {
                // commit changes
                stream_.put(reps_);
                stream_.put(prev_);
            }
            prev_ = value;
            reps_ = TVal();
        }
        reps_++;
    }

    size_t size() const {
        return stream_.size() - start_size_;
    }

    void commit() {
        stream_.put(reps_);
        stream_.put(prev_);
        stream_.commit();
    }
};

template<typename TVal>
struct RLEStreamReader {
    Base128StreamReader& stream_;
    TVal prev_;
    TVal reps_;

    RLEStreamReader(Base128StreamReader& stream)
        : stream_(stream)
        , prev_()
        , reps_()
    {}

    TVal next() {
        if (reps_ == 0) {
            reps_ = stream_.next<TVal>();
            prev_ = stream_.next<TVal>();
        }
        reps_--;
        return prev_;
    }

    unsigned char* pos() const {
        return stream_.pos_();
    }
};

struct CompressionUtil {

    /** Compress and write ChunkHeader to memory stream.
      * @param n_elements out parameter - number of written elements
      * @param ts_begin out parameter - first timestamp
      * @param ts_end out parameter - last timestamp
      * @param data ChunkHeader to compress
      */
    static
    aku_Status encode_chunk( uint32_t           *n_elements
                           , aku_Timestamp      *ts_begin
                           , aku_Timestamp      *ts_end
                           , ChunkWriter        *writer
                           , const UncompressedChunk&  data
                           );

    /** Decompress ChunkHeader.
      * @brief Decode part of the ChunkHeader structure depending on stage and steps values.
      * First goes list of timestamps, then all other values.
      * @param header out header
      * @param pbegin in - begining of the data, out - new begining of the data
      * @param end end of the data
      * @param stage current stage
      * @param steps number of stages to do
      * @param probe_length number of elements in header
      * @return current stage number
      */
    static
    aku_Status decode_chunk( UncompressedChunk   *header
                           , const unsigned char *pbegin
                           , const unsigned char  *pend
                           , uint32_t              nelements);

    /** Compress list of doubles.
      * @param input array of doubles
      * @param params array of parameter ids
      * @param buffer resulting byte array
      */
    static
    size_t compress_doubles(const std::vector<double> &input,
                                Base128StreamWriter &wstream, size_t begin=0, size_t end=0);

    /** Decompress list of doubles.
      * @param buffer input data
      * @param numbloks number of 4bit blocs inside buffer
      * @param params list of parameter ids
      * @param output resulting array
      */
    static
    void decompress_doubles(Base128StreamReader&     rstream,
                            size_t                   numvalues,
                            std::vector<double>     *output);

    /** Convert from chunk order to time order.
      * @note in chunk order all data elements ordered by series id first and then by timestamp,
      * in time order everythin ordered by time first and by id second.
      */
    static bool convert_from_chunk_order(const UncompressedChunk &header, UncompressedChunk* out);

    /** Convert from time order to chunk order.
      * @note in chunk order all data elements ordered by series id first and then by timestamp,
      * in time order everythin ordered by time first and by id second.
      */
    static bool convert_from_time_order(const UncompressedChunk &header, UncompressedChunk* out);
};

// TODO: this is obsolete, remove
// int64_t -> Delta -> ZigZag -> RLE -> Base128
typedef RLEStreamWriter<int64_t> __RLEWriter;
typedef ZigZagStreamWriter<__RLEWriter, int64_t> __ZigZagWriter;
typedef DeltaStreamWriter<__ZigZagWriter, int64_t> DeltaRLEWriter;

// TODO: this is obsolete, remove
// Base128 -> RLE -> ZigZag -> Delta -> int64_t
typedef RLEStreamReader<int64_t> __RLEReader;
typedef ZigZagStreamReader<__RLEReader, int64_t> __ZigZagReader;
typedef DeltaStreamReader<__ZigZagReader, int64_t> DeltaRLEReader;

}
