#ifndef LIBSFP_ASIO_MESSAGEQUEUE_HPP
#define LIBSFP_ASIO_MESSAGEQUEUE_HPP

#include "sfp/serial_framing_protocol.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <boost/asio/async_result.hpp>

#include <boost/log/common.hpp>
#include <boost/log/sources/logger.hpp>

#include <chrono>
#include <queue>
#include <utility>
#include <vector>

namespace sfp {
namespace asio {

namespace sys = boost::system;

/* Convert a stream object into a message queue using SFP. */
template <class Stream>
class MessageQueue {
public:
	using HandshakeHandler = std::function<void(sys::error_code&)>;
	using ReceiveHandler = std::function<void(sys::error_code&)>;
	using SendHandler = std::function<void(sys::error_code&)>;

	template <class... Args>
	explicit MessageQueue (Args&&... args)
			: mStream(std::forward<Args>(args)...)
			, mSfpTimer(mStream.get_io_service())
			, mStrand(mStream.get_io_service()) { }

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (std::string debugName) {
		sfpSetDebugName(&mContext, debugName.c_str());
	}
#endif

	Stream& stream () { return mStream; }
	const Stream& stream () const { return mStream; }

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncHandshake (Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		mStrand.dispatch([this, realHandler] () mutable {
			cancel();
			mHandshakeHandler = realHandler;
			handshakeWriteCoroutine();
			handshakeReadCoroutine();
		});

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncShutdown (Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		mStrand.dispatch([this, realHandler] () mutable {
			cancel();
			mStream.get_io_service().post(std::bind(realHandler, sys::error_code()));
		});

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncSend (const boost::asio::const_buffer& buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		mStrand.dispatch([this, &buffer, realHandler] () mutable {
			size_t outlen;
			sfpWritePacket(&mContext,
				boost::asio::buffer_cast<const uint8_t*>(buffer),
				boost::asio::buffer_size(buffer), &outlen);
			flushWriteBuffer(realHandler);
		});

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncReceive (const boost::asio::mutable_buffer& buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		mStrand.dispatch([this, &buffer, realHandler] () mutable {
			mReceives.emplace(std::make_pair(buffer, realHandler));
			postReceives();
		});

		return init.result.get();
	}

	void cancel () {
		reset(make_error_code(boost::asio::error::operation_aborted));
	}

private:
	void reset (boost::system::error_code ec) {
		mStream.cancel();
		mSfpTimer.cancel();

		auto resetState = [this, ec] () {
			postReceives();
			voidAllHandlers(ec);

			mInbox = decltype(mInbox)();
			mWriteBuffer.clear();

			sfpInit(&mContext);
			sfpSetWriteCallback(&mContext, SFP_WRITE_MULTIPLE,
				(void*)writeCallback, this);
			sfpSetDeliverCallback(&mContext, deliverCallback, this);
		};

		// Guarantee that if we're called in the strand, then the state will
		// be reset when we return.
		if (mStrand.running_in_this_thread()) {
			resetState();
		}
		else {
			mStrand.dispatch(resetState);
		}
	}
	
	void readAndWrite (boost::asio::yield_context yield) {
		uint8_t buf[256];
		auto nRead = mStream.async_read_some(boost::asio::buffer(buf), yield);
		for (size_t i = 0; i < nRead; ++i) {
			auto rc = sfpDeliverOctet(&mContext, buf[i], nullptr, 0, nullptr);
			assert(-1 != rc);
		}
		flushWriteBuffer([] (sys::error_code) { });
	}

	void handshakeWriteCoroutine () {
		BOOST_LOG(mLog) << "Spawning handshake write coroutine";
		boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
			try {
				do {
					BOOST_LOG(mLog) << "Sending sfp connect packet";
					sfpConnect(&mContext);
					flushWriteBuffer([] (sys::error_code) { });
					mSfpTimer.expires_from_now(kSfpConnectTimeout);
					mSfpTimer.async_wait(yield);
				} while (!sfpIsConnected(&mContext));
				BOOST_LOG(mLog) << "sfp appears to be connected";
				mSfpTimer.expires_from_now(kSfpSettleTimeout);
				mSfpTimer.async_wait(yield);
				BOOST_LOG(mLog) << "sfp connection is settled";
				assert(mHandshakeHandler);
				mStream.get_io_service().post(
					std::bind(mHandshakeHandler, sys::error_code())
				);
				mHandshakeHandler = nullptr;
			}
			catch (sys::system_error& e) {
				reset(e.code());
			}
		});
	}

	void handshakeReadCoroutine () {
		boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
			try {
				while (true) {
					postReceives();
					readAndWrite(yield);
				}
			}
			catch (sys::system_error& e) {
				reset(e.code());
			}
		});
	}

	void postReceives () {
		BOOST_LOG(mLog) << "mInbox.size() == " << mInbox.size() << " | mReceives.size() == "
						<< mReceives.size();
		while (mInbox.size() && mReceives.size()) {
			auto nCopied = boost::asio::buffer_copy(mReceives.front().first,
				boost::asio::buffer(mInbox.front()));
			
			auto ec = nCopied <= boost::asio::buffer_size(mReceives.front().first)
					  ? sys::error_code()
					  : make_error_code(boost::asio::error::message_size);

			mStream.get_io_service().post(std::bind(mReceives.front().second, ec));
			mInbox.pop();
			mReceives.pop();
		}
	}

	void voidAllHandlers (boost::system::error_code ec) {
		if (mHandshakeHandler) {
			mStream.get_io_service().post(std::bind(mHandshakeHandler, ec));
			mHandshakeHandler = nullptr;
		}

		while (mReceives.size()) {
			mStream.get_io_service().post(std::bind(mReceives.front().second, ec));
			mReceives.pop();
		}

		while (mOutbox.size()) {
			mStream.get_io_service().post(std::bind(mOutbox.front().second, ec));
			mOutbox.pop();
		}
	}

	template <class Handler>
	void flushWriteBuffer (Handler handler) {
		if (!mWriteBuffer.size()) {
			mStream.get_io_service().post(std::bind(handler, sys::error_code()));
			return;
		}
		mOutbox.emplace(std::make_pair(mWriteBuffer, handler));
		mWriteBuffer.clear();
		if (1 == mOutbox.size()) {
			BOOST_LOG(mLog) << "spawning writer coroutine";
			boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
				try {
					do {
						boost::asio::async_write(mStream, boost::asio::buffer(mOutbox.front().first), yield);
						mStream.get_io_service().post(
							std::bind(mOutbox.front().second, sys::error_code())
						);
						mOutbox.pop();
					} while (mOutbox.size());
				}
				catch (boost::system::system_error& e) {
					reset(e.code());
				}
			});
		}
	}

	// pushes octets onto a vector, a write buffer
	static int writeCallback (uint8_t* octets, size_t len, size_t* outlen, void* data) {
		auto& writeBuffer = static_cast<MessageQueue*>(data)->mWriteBuffer;
		writeBuffer.insert(writeBuffer.end(), octets, octets + len);
		if (outlen) { *outlen = len; }
		return 0;
	}

	// Receive one message. Push onto a queue of incoming messages.
	static void deliverCallback (uint8_t* buf, size_t len, void* data) {
		static_cast<MessageQueue*>(data)->mInbox.emplace(buf, buf + len);
	}

	static const std::chrono::milliseconds kSfpConnectTimeout;
	static const std::chrono::milliseconds kSfpSettleTimeout;

	mutable boost::log::sources::logger mLog;

	std::queue<std::vector<uint8_t>> mInbox;
	std::queue<std::pair<boost::asio::mutable_buffer, ReceiveHandler>> mReceives;

	std::vector<uint8_t> mWriteBuffer;
	std::queue<std::pair<std::vector<uint8_t>, SendHandler>> mOutbox;

	HandshakeHandler mHandshakeHandler;

	Stream mStream;
	boost::asio::steady_timer mSfpTimer;
	boost::asio::io_service::strand mStrand;

	SFPcontext mContext;
};

template <class Stream>
const std::chrono::milliseconds MessageQueue<Stream>::kSfpConnectTimeout { 100 };
//std::chrono::milliseconds(100);

template <class Stream>
const std::chrono::milliseconds MessageQueue<Stream>::kSfpSettleTimeout { 200 };
//std::chrono::milliseconds(200);

} // namespace asio
} // namespace sfp

#endif