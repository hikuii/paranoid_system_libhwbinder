/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef HIDL_MQ_H
#define HIDL_MQ_H

#include <android-base/logging.h>
#include <hidl/MQDescriptor.h>
#include <sys/mman.h>
#include <atomic>

namespace android {
namespace hardware {

template <typename T>
struct MessageQueue {
  MessageQueue(const MQDescriptor& Desc);
  ~MessageQueue();

  size_t availableToWrite() const;
  size_t availableToRead() const;
  size_t getQuantumSize() const;
  size_t getQuantumCount() const;
  bool isValid() const;

  bool write(const T* data);
  bool read(T* data);
  bool write(const T* data, size_t count);
  bool read(T* data, size_t count);
  const MQDescriptor* getDesc() const { return &mDesc; }

 private:
  struct region {
    uint8_t* address;
    size_t length;
  };
  struct transaction {
    region first;
    region second;
  };

  size_t writeBytes(const uint8_t* data, size_t size);
  transaction beginWrite(size_t nBytesDesired) const;
  void commitWrite(size_t nBytesWritten);

  size_t readBytes(uint8_t* data, size_t size);
  transaction beginRead(size_t nBytesDesired) const;
  void commitRead(size_t nBytesRead);

  MessageQueue(const MessageQueue& other) = delete;
  MessageQueue& operator=(const MessageQueue& other) = delete;
  MessageQueue();

  void* mapGrantorDescr(uint32_t grantor_idx);
  void unmapGrantorDescr(void* address, uint32_t grantor_idx);
  MQDescriptor mDesc;
  uint8_t* mRing;
  std::atomic<uint64_t>* mReadPtr;
  std::atomic<uint64_t>* mWritePtr;
};

template <typename T>
MessageQueue<T>::MessageQueue(const MQDescriptor& Desc) : mDesc(Desc) {
  /*
   * Verify that the the Descriptor contains the minimum number of grantors
   * the native_handle is valid and T matches quantum size.
   */
  if (!Desc.isHandleValid() ||
      (Desc.countGrantors() < MQDescriptor::kMinGrantorCount) ||
      (Desc.getQuantum() != sizeof(T))) {
    return;
  }

  mReadPtr =
      reinterpret_cast<std::atomic<uint64_t>*>(mapGrantorDescr
                                               (MQDescriptor::READPTRPOS));
  CHECK(mReadPtr != nullptr);

  mWritePtr =
      reinterpret_cast<std::atomic<uint64_t>*>(mapGrantorDescr
                                               (MQDescriptor::WRITEPTRPOS));
  CHECK(mWritePtr != nullptr);

  mReadPtr->store(0, std::memory_order_acquire);
  mWritePtr->store(0, std::memory_order_acquire);

  mRing = reinterpret_cast<uint8_t*>(mapGrantorDescr
                                     (MQDescriptor::DATAPTRPOS));
  CHECK(mRing != nullptr);
}

template <typename T>
MessageQueue<T>::~MessageQueue() {
  if (mReadPtr) unmapGrantorDescr(mReadPtr, MQDescriptor::READPTRPOS);
  if (mWritePtr) unmapGrantorDescr(mWritePtr, MQDescriptor::WRITEPTRPOS);
  if (mRing) unmapGrantorDescr(mRing, MQDescriptor::DATAPTRPOS);
}
template <typename T>
bool MessageQueue<T>::write(const T* data) {
  return write(data, 1);
}

template <typename T>
bool MessageQueue<T>::read(T* data) {
  return read(data, 1);
}
template <typename T>
bool MessageQueue<T>::write(const T* data, size_t count) {
  if (availableToWrite() < sizeof(T) * count) {
    return false;
  }

  return (writeBytes(reinterpret_cast<const uint8_t*>(data),
                     sizeof(T) * count) == sizeof(T) * count);
}

template <typename T>
bool MessageQueue<T>::read(T* data, size_t count) {
  if (availableToRead() < sizeof(T) * count) {
    return false;
  }
  return readBytes(reinterpret_cast<uint8_t*>(data), sizeof(T) * count) ==
         sizeof(T) * count;
}

template <typename T>
size_t MessageQueue<T>::availableToWrite() const {
  return mDesc.getSize() - availableToRead();
}

template <typename T>
size_t MessageQueue<T>::writeBytes(const uint8_t* data, size_t size) {
  transaction tx = beginWrite(size);
  memcpy(tx.first.address, data, tx.first.length);
  memcpy(tx.second.address, data + tx.first.length, tx.second.length);
  size_t result = tx.first.length + tx.second.length;
  commitWrite(result);
  return result;
}

/*
 * The below method does not check for available space since it was already
 * checked by write() API which invokes writeBytes() which in turn calls
 * beginWrite().
 */
template <typename T>
typename MessageQueue<T>::transaction MessageQueue<T>::beginWrite(
    size_t nBytesDesired) const {
  transaction result;
  auto readPtr = mReadPtr->load(std::memory_order_acquire);
  auto writePtr = mWritePtr->load(std::memory_order_relaxed);
  size_t writeOffset = writePtr % mDesc.getSize();
  size_t contiguous = mDesc.getSize() - writeOffset;
  if (contiguous < nBytesDesired) {
    result = {{mRing + writeOffset, contiguous},
              {mRing, nBytesDesired - contiguous}};
  } else {
    result = {
        {mRing + writeOffset, nBytesDesired}, {0, 0},
    };
  }
  return result;
}

template <typename T>
void MessageQueue<T>::commitWrite(size_t nBytesWritten) {
  auto writePtr = mWritePtr->load(std::memory_order_relaxed);
  writePtr += nBytesWritten;
  mWritePtr->store(writePtr, std::memory_order_release);
}

template <typename T>
size_t MessageQueue<T>::availableToRead() const {
  /*
   * Doing relaxed loads here because these accesses don't carry dependencies.
   * Dependent accesses won't happen until after a call to beginWrite or
   * beginRead
   * which do proper acquire/release.
   */
  return mWritePtr->load(std::memory_order_relaxed) -
         mReadPtr->load(std::memory_order_relaxed);
}

template <typename T>
size_t MessageQueue<T>::readBytes(uint8_t* data, size_t size) {
  transaction tx = beginRead(size);
  memcpy(data, tx.first.address, tx.first.length);
  memcpy(data + tx.first.length, tx.second.address, tx.second.length);
  size_t result = tx.first.length + tx.second.length;
  commitRead(result);
  return result;
}

/*
 * The below method does not check whether nBytesDesired bytes are available
 * to read because the check is performed in the read() method before
 * readBytes() is invoked.
 */
template <typename T>
typename MessageQueue<T>::transaction MessageQueue<T>::beginRead(
    size_t nBytesDesired) const {
  transaction result;
  auto writePtr = mWritePtr->load(std::memory_order_acquire);
  auto readPtr = mReadPtr->load(std::memory_order_relaxed);

  size_t readOffset = readPtr % mDesc.getSize();
  size_t contiguous = mDesc.getSize() - readOffset;

  if (contiguous < nBytesDesired) {
    result = {{mRing + readOffset, contiguous},
              {mRing, nBytesDesired - contiguous}};
  } else {
    result = {
        {mRing + readOffset, nBytesDesired}, {0, 0},
    };
  }

  return result;
}

template <typename T>
void MessageQueue<T>::commitRead(size_t nBytesRead) {
  auto readPtr = mReadPtr->load(std::memory_order_relaxed);
  readPtr += nBytesRead;
  mReadPtr->store(readPtr, std::memory_order_release);
}

template <typename T>
size_t MessageQueue<T>::getQuantumSize() const {
  return mDesc.getQuantum();
}

template <typename T>
size_t MessageQueue<T>::getQuantumCount() const {
  return mDesc.getSize() / mDesc.getQuantum();
}

template <typename T>
bool MessageQueue<T>::isValid() const {
  return mRing != nullptr && mReadPtr != nullptr && mWritePtr != nullptr;
}

template <typename T>
void* MessageQueue<T>::mapGrantorDescr(uint32_t grantor_idx) {
  const native_handle_t* handle = mDesc.getNativeHandle()->handle();
  auto mGrantors = mDesc.getGrantors();
  int fdIndex = mGrantors[grantor_idx].fdIndex;
  /*
   * Offset for mmap must be a multiple of PAGE_SIZE.
   */
  int mapOffset = (mGrantors[grantor_idx].offset / PAGE_SIZE) * PAGE_SIZE;
  int mapLength =
      mGrantors[grantor_idx].offset - mapOffset + mGrantors[grantor_idx].extent;

  void* address = mmap(0, mapLength, PROT_READ | PROT_WRITE, MAP_SHARED,
                       handle->data[fdIndex], mapOffset);
  return (address == MAP_FAILED)
             ? nullptr
             : reinterpret_cast<uint8_t*>(address) +
             (mGrantors[grantor_idx].offset - mapOffset);
}

template <typename T>
void MessageQueue<T>::unmapGrantorDescr(void* address, uint32_t grantor_idx) {
  const native_handle_t* handle = mDesc.getNativeHandle()->handle();
  auto mGrantors = mDesc.getGrantors();
  int mapOffset = (mGrantors[grantor_idx].offset / PAGE_SIZE) * PAGE_SIZE;
  int mapLength =
      mGrantors[grantor_idx].offset - mapOffset + mGrantors[grantor_idx].extent;
  void* baseAddress = reinterpret_cast<uint8_t*>(address) -
                      (mGrantors[grantor_idx].offset - mapOffset);
  if (baseAddress) munmap(baseAddress, mapLength);
}

}  // namespace hardware
}  // namespace android
#endif  // HIDL_MQ_H