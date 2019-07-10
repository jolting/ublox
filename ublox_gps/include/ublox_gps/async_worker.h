//==============================================================================
// Copyright (c) 2012, Johannes Meyer, TU Darmstadt
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the Flight Systems and Automatic Control group,
//       TU Darmstadt, nor the names of its contributors may be used to
//       endorse or promote products derived from this software without
//       specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//==============================================================================

#ifndef UBLOX_GPS_ASYNC_WORKER_H
#define UBLOX_GPS_ASYNC_WORKER_H

#include <ublox_gps/gps.h>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition.hpp>


#include "worker.h"

namespace ublox_gps {

int debug; //!< Used to determine which debug messages to display

/**
 * @brief Handles Asynchronous I/O reading and writing.
 */
template <typename StreamT>
class AsyncWorker : public Worker {
  public:
  using Callback = boost::function<void(unsigned char*, std::size_t)>;
  /**
   * @brief Construct an Asynchronous I/O worker.
   * @param stream the stream for th I/O service
   * @param io_service the I/O service
   * @param buffer_size the size of the input and output buffers
   */
  AsyncWorker(boost::shared_ptr<StreamT> stream,
              boost::shared_ptr<boost::asio::io_service> io_service,
	      const Callback& read_callback,
	      const Callback& write_callback = Callback(),
	      std::size_t buffer_size = 8192);
  virtual ~AsyncWorker();

  /**
   * @brief Send the data bytes via the I/O stream.
   * @param data the buffer of data bytes to send
   * @param size the size of the buffer
   */
  bool send(const unsigned char* data, const unsigned int size);

 protected:
  /**
   * @brief Read the input stream.
   */
  void doReadSyncWord();
  void doReadSyncWordPartial();
  void doReadHeader();
  void doReadMsg(size_t message_size);

  /**
   * @brief Process messages read from the input stream.
   * @param error_code an error code for read failures
   * @param the number of bytes received
   */
  void readEndSyncWord(const boost::system::error_code&, std::size_t);
  void readEndHeader(const boost::system::error_code&, std::size_t);
  void readEndMsg(const boost::system::error_code&, std::size_t);

  /**
   * @brief Send all the data in the output buffer.
   */
  void doWrite();

  /**
   * @brief Close the I/O stream.
   */
  void doClose();

  boost::shared_ptr<StreamT> stream_; //!< The I/O stream
  boost::shared_ptr<boost::asio::io_service> io_service_; //!< The I/O service

  std::vector<unsigned char> in_; //!< The input buffer

  std::vector<unsigned char> out_; //!< The output buffer

  boost::shared_ptr<boost::thread> background_thread_; //!< thread for the I/O
                                                       //!< service
  Callback read_callback_; //!< Callback function to handle received messages
  Callback write_callback_; //!< Callback function to handle raw data
};

template <typename StreamT>
AsyncWorker<StreamT>::AsyncWorker(boost::shared_ptr<StreamT> stream,
        boost::shared_ptr<boost::asio::io_service> io_service,
        const Callback& read_callback,
        const Callback& write_callback,
	std::size_t buffer_size)
    : stream_(stream),
      io_service_(io_service),
      in_(buffer_size),
      read_callback_(read_callback),
      write_callback_(write_callback)
{
  out_.reserve(buffer_size),
  io_service_->post(boost::bind(&AsyncWorker<StreamT>::doReadHeader, this));
  background_thread_.reset(new boost::thread(
      boost::bind(&boost::asio::io_service::run, io_service_)));
}

template <typename StreamT>
AsyncWorker<StreamT>::~AsyncWorker() {
  io_service_->stop();
  background_thread_->join();
  stream_->close();
}

template <typename StreamT>
bool AsyncWorker<StreamT>::send(const unsigned char* data,
                                const unsigned int size) {
  if(size == 0) {
    ROS_ERROR("Ublox AsyncWorker::send: Size of message to send is 0");
    return true;
  }

  if (out_.capacity() - out_.size() < size) {
    ROS_ERROR("Ublox AsyncWorker::send: Output buffer too full to send message");
    return false;
  }
  out_.insert(out_.end(), data, data + size);

  io_service_->post(boost::bind(&AsyncWorker<StreamT>::doWrite, this));
  return true;
}

template <typename StreamT>
void AsyncWorker<StreamT>::doWrite() {
  // Do nothing if out buffer is empty
  if (out_.size() == 0) {
    return;
  }
  // Write all the data in the out buffer
  boost::asio::write(*stream_, boost::asio::buffer(out_.data(), out_.size()));

  // Clear the buffer & unlock
  out_.clear();
}

template <typename StreamT>
void AsyncWorker<StreamT>::doReadSyncWord() {
  async_read(*stream_,
      boost::asio::buffer(in_.data(), 2),
      boost::asio::transfer_exactly(2),
                          boost::bind(&AsyncWorker<StreamT>::readEndSyncWord, this,
                              boost::asio::placeholders::error,
                              boost::asio::placeholders::bytes_transferred));
}

template <typename StreamT>
void AsyncWorker<StreamT>::doReadSyncWordPartial() {
  in_[0] = in_[1]; // handles the partial case. Overwritten otherwise
  async_read(*stream_,
      boost::asio::buffer(in_.data() + 1, 1),
      boost::asio::transfer_exactly(1),
                          boost::bind(&AsyncWorker<StreamT>::readEndSyncWord, this,
                              boost::asio::placeholders::error,
                              boost::asio::placeholders::bytes_transferred));
}


template <typename StreamT>
void AsyncWorker<StreamT>::doReadHeader() {
  async_read(*stream_,
      boost::asio::buffer(in_.data() + 2, 4),
      boost::asio::transfer_exactly(4),
                          boost::bind(&AsyncWorker<StreamT>::readEndHeader, this,
                              boost::asio::placeholders::error,
                              boost::asio::placeholders::bytes_transferred));
}

template <typename StreamT>
void AsyncWorker<StreamT>::doReadMsg(size_t messageSize){
  async_read(*stream_,
      boost::asio::buffer(in_.data() + 6, messageSize),
      boost::asio::transfer_exactly(messageSize),
                          boost::bind(&AsyncWorker<StreamT>::readEndMsg, this,
                              boost::asio::placeholders::error,
                              boost::asio::placeholders::bytes_transferred));
}

template <typename StreamT>
void AsyncWorker<StreamT>::readEndSyncWord(const boost::system::error_code& error,
                                   std::size_t bytes_transfered) {
  if (error) {
    ROS_ERROR("U-Blox ASIO input buffer read error: %s, %li",
              error.message().c_str(),
              bytes_transfered);
  } else {
    if (in_[0] != 0xB5) {
      if(in_[1] == 0xB5)
         doReadSyncWordPartial(); 
      else
         doReadSyncWord();
      return;
    }
    if (in_[1] != 0x62) {
      doReadSyncWord(); // do full read for sync word
      return;
    }
    if (write_callback_)
      write_callback_(in_.data(), bytes_transfered);
    doReadHeader();
  }
}


template <typename StreamT>
void AsyncWorker<StreamT>::readEndHeader(const boost::system::error_code& error,
                                   std::size_t bytes_transfered) {
  if (write_callback_)
    write_callback_(in_.data(), bytes_transfered);

  uint16_t size = in_[4] << 8 + in_[3];
  //ToDo Verify that the 4 bytes we just read are sane. Message Type? Message Length?
  doReadMsg(size + 2); //Read the checksum + message size
}

template <typename StreamT>
void AsyncWorker<StreamT>::readEndMsg(const boost::system::error_code& error,
                                   std::size_t bytes_transfered) {
  if (error) {
    ROS_ERROR("U-Blox ASIO input buffer read error: %s, %li",
              error.message().c_str(),
              bytes_transfered);
  } else {
    if (write_callback_)
      write_callback_(in_.data(), bytes_transfered);
    if (read_callback_)
      read_callback_(in_.data(), bytes_transfered + 6 + 2); // Header + message + checksum
    doReadSyncWord(); // Read the next sync word
  }
}

}  // namespace ublox_gps

#endif  // UBLOX_GPS_ASYNC_WORKER_H
