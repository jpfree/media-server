#include "MsRtcSource.h"
#include "MsMsg.h"
#include "MsReactor.h"
#include "MsResManager.h"
#include <sstream>

MsRtcSource::~MsRtcSource() {
	MS_LOG_INFO("~MsRtcSource %s", _sessionId.c_str());

	while (m_videoRtpQue.size()) {
		m_videoRtpQue.pop();
	}
	while (m_videoPool.size()) {
		m_videoPool.pop();
	}
	while (m_audioRtpQue.size()) {
		m_audioRtpQue.pop();
	}
	while (m_audioPool.size()) {
		m_audioPool.pop();
	}
}

void MsRtcSource::AddSink(std::shared_ptr<MsMediaSink> sink) {
	if (!sink || m_isClosing.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(m_sinkMutex);
	m_sinks.push_back(sink);
	if (m_video || m_audio) {
		sink->OnStreamInfo(m_video, m_videoIdx, m_audio, m_videoIdx + 1);
	}
	if (_videoTrack && _pc) {
		try {
			_videoTrack->requestKeyframe();
		} catch (...) {
		}
	}
}

void MsRtcSource::NotifyStreamPacket(AVPacket *pkt) {
	std::lock_guard<std::mutex> lock(m_sinkMutex);
	for (auto &sink : m_sinks) {
		if (sink) {
			sink->OnStreamPacket(pkt);
		}
	}
}

void MsRtcSource::SourceActiveClose() {
	m_isClosing.store(true);
	m_videoCondVar.notify_all();

	try {
		_pc->close();
	} catch (...) {
	}

	if (m_rtpThread) {
		if (m_rtpThread->joinable())
			m_rtpThread->join();
		m_rtpThread.reset();
	}

	if (_sock) {
		_sock.reset();
	}

	MsMediaSource::SourceActiveClose();
}

string MsRtcSource::GenVideoSdp() {
	std::stringstream ss;
	ss << "v=0\r\n";
	ss << "o=- 0 0 IN IP4 127.0.0.1\r\n";
	ss << "c=IN IP4 127.0.0.1\r\n";

	if (_videoPt > 0) {
		ss << "m=video 0 RTP/AVP " << _videoPt << "\r\n";
		ss << "a=rtpmap:" << _videoPt << " " << _videoCodec << "/90000\r\n";
		for (size_t i = 0; i < _videoFmts.size(); ++i) {
			ss << "a=fmtp:" << _videoPt << " " << _videoFmts[i];
			ss << "\r\n";
		}
	}

	return ss.str();
}

string MsRtcSource::GenAudioSdp() {
	std::stringstream ss;
	ss << "v=0\r\n";
	ss << "o=- 0 0 IN IP4 127.0.0.1\r\n";
	ss << "c=IN IP4 127.0.0.1\r\n";

	if (_audioPt > 0) {
		ss << "m=audio 0 RTP/AVP " << _audioPt << "\r\n";
		int clockRate = 48000;
		ss << "a=rtpmap:" << _audioPt << " " << _audioCodec << "/" << clockRate << "/2\r\n";
		for (size_t i = 0; i < _audioFmts.size(); ++i) {
			ss << "a=fmtp:" << _audioPt << " " << _audioFmts[i];
			ss << "\r\n";
		}
	}

	return ss.str();
}

void MsRtcSource::StartRtpDemux() {
	int rtp_buff_size = 1500;
	AVIOContext *rtp_video_context = nullptr;
	AVIOContext *rtp_audio_context = nullptr;
	AVIOContext *sdp_avio_context = nullptr;
	AVFormatContext *videoFmtCtx = nullptr;
	AVFormatContext *audioFmtCtx = nullptr;
	AVPacket *pkt = nullptr;
	bool firstVideoPkt = true;
	unique_ptr<thread> audioThread;

	_sdp = this->GenVideoSdp();
	MsResManager::GetInstance().AddMediaSource(_sessionId, this->GetSharedPtr());

	sdp_avio_context = avio_alloc_context(
	    static_cast<unsigned char *>(av_malloc(_sdp.size())), _sdp.size(), 0, this,
	    [](void *opaque, uint8_t *buf, int buf_size) -> int {
		    auto rpc = static_cast<MsRtcSource *>(opaque);
		    if (rpc->_readSdpPos >= (int)rpc->_sdp.size()) {
			    return AVERROR_EOF;
		    }

		    int left = rpc->_sdp.size() - rpc->_readSdpPos;
		    if (buf_size > left) {
			    buf_size = left;
		    }

		    memcpy(buf, rpc->_sdp.c_str() + rpc->_readSdpPos, buf_size);
		    rpc->_readSdpPos += buf_size;
		    return buf_size;
	    },
	    NULL, NULL);

	rtp_video_context = avio_alloc_context(
	    static_cast<unsigned char *>(av_malloc(rtp_buff_size)), rtp_buff_size, 1, this,
	    [](void *opaque, uint8_t *buf, int buf_size) -> int {
		    // This is RTP Packet
		    auto rpc = static_cast<MsRtcSource *>(opaque);
		    return rpc->ReadBuffer(buf, buf_size, true);
	    },
	    // Ignore RTCP Packets. Must be set
	    [](void *, IO_WRITE_BUF_TYPE *, int buf_size) -> int { return buf_size; }, NULL);

	videoFmtCtx = avformat_alloc_context();
	videoFmtCtx->pb = sdp_avio_context;

	AVDictionary *options = nullptr;
	av_dict_set(&options, "sdp_flags", "custom_io", 0);
	av_dict_set_int(&options, "reorder_queue_size", 0, 0);
	av_dict_set(&options, "protocol_whitelist", "file,rtp,udp", 0);

	int ret = avformat_open_input(&videoFmtCtx, "", nullptr, &options);
	av_dict_free(&options);

	if (ret != 0) {
		MS_LOG_ERROR("pc:%s avformat_open_input failed:%d", _sessionId.c_str(), ret);
		goto err;
	}

	// release sdp avio context
	av_freep(&sdp_avio_context->buffer);
	avio_context_free(&sdp_avio_context);
	sdp_avio_context = nullptr;

	videoFmtCtx->pb = rtp_video_context;

	if ((ret = avformat_find_stream_info(videoFmtCtx, nullptr)) != 0) {
		// log error
		MS_LOG_ERROR("pc:%s avformat_find_stream_info failed:%d", _sessionId.c_str(), ret);
		goto err;
	}

	ret = av_find_best_stream(videoFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (ret < 0) {
		MS_LOG_ERROR("pc:%s no video stream found", _sessionId.c_str());
		goto err;
	}
	m_videoIdx = ret;
	m_video = videoFmtCtx->streams[m_videoIdx];

	// for audio, use a different format context
	if (_audioPt > 0) {
		_sdp = this->GenAudioSdp();
		_readSdpPos = 0;
		sdp_avio_context = avio_alloc_context(
		    static_cast<unsigned char *>(av_malloc(_sdp.size())), _sdp.size(), 0, this,
		    [](void *opaque, uint8_t *buf, int buf_size) -> int {
			    auto rpc = static_cast<MsRtcSource *>(opaque);
			    if (rpc->_readSdpPos >= (int)rpc->_sdp.size()) {
				    return AVERROR_EOF;
			    }

			    int left = rpc->_sdp.size() - rpc->_readSdpPos;
			    if (buf_size > left) {
				    buf_size = left;
			    }

			    memcpy(buf, rpc->_sdp.c_str() + rpc->_readSdpPos, buf_size);
			    rpc->_readSdpPos += buf_size;
			    return buf_size;
		    },
		    NULL, NULL);

		rtp_audio_context = avio_alloc_context(
		    static_cast<unsigned char *>(av_malloc(rtp_buff_size)), rtp_buff_size, 1, this,
		    [](void *opaque, uint8_t *buf, int buf_size) -> int {
			    // This is RTP Packet
			    auto rpc = static_cast<MsRtcSource *>(opaque);
			    return rpc->ReadBuffer(buf, buf_size, false);
		    },
		    // Ignore RTCP Packets. Must be set
		    [](void *, IO_WRITE_BUF_TYPE *, int buf_size) -> int { return buf_size; }, NULL);

		audioFmtCtx = avformat_alloc_context();
		audioFmtCtx->pb = sdp_avio_context;

		av_dict_set(&options, "sdp_flags", "custom_io", 0);
		av_dict_set_int(&options, "reorder_queue_size", 0, 0);
		av_dict_set(&options, "protocol_whitelist", "file,rtp,udp", 0);

		ret = avformat_open_input(&audioFmtCtx, "", nullptr, &options);
		av_dict_free(&options);

		if (ret != 0) {
			MS_LOG_ERROR("pc:%s avformat_open_input for audio failed:%d", _sessionId.c_str(), ret);
			goto err;
		}

		// release sdp avio context
		av_freep(&sdp_avio_context->buffer);
		avio_context_free(&sdp_avio_context);
		sdp_avio_context = nullptr;

		audioFmtCtx->pb = rtp_audio_context;

		ret = avformat_find_stream_info(audioFmtCtx, nullptr);
		if (ret != 0) {
			MS_LOG_ERROR("pc:%s avformat_find_stream_info for audio failed:%d", _sessionId.c_str(),
			             ret);
			goto err;
		}

		ret = av_find_best_stream(audioFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (ret >= 0) {
			AVCodecParameters *codecPar = audioFmtCtx->streams[ret]->codecpar;
			if (codecPar->codec_id == AV_CODEC_ID_OPUS || codecPar->codec_id == AV_CODEC_ID_AAC) {
				m_audioIdx = ret;
				m_audio = audioFmtCtx->streams[m_audioIdx];
			} else {
				MS_LOG_ERROR("pc:%s unsupported audio codec:%d", _sessionId.c_str(),
				             codecPar->codec_id);
			}
		} else {
			MS_LOG_ERROR("pc:%s no audio stream found", _sessionId.c_str());
		}
	}

	this->NotifyStreamInfo();

	// for audio demux, use a separate thread to avoid blocking video demux
	if (m_audio) {
		audioThread = std::make_unique<thread>([this, audioFmtCtx]() {
			AVPacket *audioPkt = av_packet_alloc();
			while (av_read_frame(audioFmtCtx, audioPkt) >= 0 && !m_isClosing.load()) {
				if (audioPkt->stream_index == m_audioIdx) {
					audioPkt->stream_index = m_videoIdx + 1; // audio stream index is videoIdx + 1
					this->NotifyStreamPacket(audioPkt);
					audioPkt->stream_index = m_audioIdx;
				}
				av_packet_unref(audioPkt);
			}

			av_packet_free(&audioPkt);
		});
	}

	pkt = av_packet_alloc();

	/* read frames from the file */
	while (av_read_frame(videoFmtCtx, pkt) >= 0 && !m_isClosing.load()) {
		if (pkt->stream_index == m_videoIdx) {
			/*if (pkt->stream_index == m_videoIdx) {
			    MS_LOG_DEBUG("rtc source video pkt pts:%ld dts:%ld key:%d", pkt->pts, pkt->dts,
			                 pkt->flags & AV_PKT_FLAG_KEY);
			} */

			if (firstVideoPkt) {
				firstVideoPkt = false;
				// TODO: quick fix for some RTSP stream with first pkt
				// pts=dts=AV_NOPTS_VALUE
				//       need better solution
				if (pkt->pts == AV_NOPTS_VALUE) {
					MS_LOG_WARN("first video pkt pts is AV_NOPTS_VALUE, set to 0");
					pkt->pts = 0;
					if (pkt->dts == AV_NOPTS_VALUE) {
						pkt->dts = 0;
					}
				}
			}

			this->NotifyStreamPacket(pkt);
		}
		av_packet_unref(pkt);
	}

err:
	if (videoFmtCtx) {
		if (videoFmtCtx->pb) {
			av_freep(&videoFmtCtx->pb->buffer);
			avio_context_free(&videoFmtCtx->pb);
		}
		avformat_free_context(videoFmtCtx);
		videoFmtCtx = nullptr;
	}

	av_packet_free(&pkt);

	if (audioThread && audioThread->joinable()) {
		m_isClosing.store(true);
		m_audioCondVar.notify_all();
		audioThread->join();
		audioThread.reset();
	}

	if (audioFmtCtx) {
		if (audioFmtCtx->pb) {
			av_freep(&audioFmtCtx->pb->buffer);
			avio_context_free(&audioFmtCtx->pb);
		}
		avformat_free_context(audioFmtCtx);
		audioFmtCtx = nullptr;
	}

	MsMsg msg;
	msg.m_msgID = MS_RTC_PEER_CLOSED;
	msg.m_strVal = _sessionId;
	msg.m_dstType = MS_RTC_SERVER;
	msg.m_dstID = 1;
	MsReactorMgr::Instance()->PostMsg(msg);
}

void MsRtcSource::WriteBuffer(const void *buf, int size, bool isVideo) {
	std::mutex &mtx = isVideo ? m_videoQueMtx : m_audioQueMtx;
	std::condition_variable &condVar = isVideo ? m_videoCondVar : m_audioCondVar;
	std::queue<SData> &rtpQue = isVideo ? m_videoRtpQue : m_audioRtpQue;
	std::queue<SData> &pool = isVideo ? m_videoPool : m_audioPool;

	std::unique_lock<std::mutex> lock(mtx);

	if (pool.size() > 0) {
		SData &sd = pool.front();
		if (sd.m_capacity >= size) {
			memcpy(sd.m_uBuf.get(), buf, size);
			sd.m_len = size;
			rtpQue.emplace(std::move(sd));
			pool.pop();
			lock.unlock();
			condVar.notify_one();
			return;
		} else {
			// not enough capacity
			pool.pop();
		}
	}

	SData sd;
	sd.m_uBuf = std::make_unique<uint8_t[]>(size);
	sd.m_len = size;
	sd.m_capacity = size;
	memcpy(sd.m_uBuf.get(), buf, size);
	rtpQue.emplace(std::move(sd));
	lock.unlock();
	condVar.notify_one();
}

int MsRtcSource::ReadBuffer(uint8_t *buf, int buf_size, bool isVideo) {
	std::mutex &mtx = isVideo ? m_videoQueMtx : m_audioQueMtx;
	std::condition_variable &condVar = isVideo ? m_videoCondVar : m_audioCondVar;
	std::queue<SData> &rtpQue = isVideo ? m_videoRtpQue : m_audioRtpQue;
	std::queue<SData> &pool = isVideo ? m_videoPool : m_audioPool;

	std::unique_lock<std::mutex> lock(mtx);
	condVar.wait(lock, [this, &rtpQue]() { return rtpQue.size() > 0 || m_isClosing.load(); });

	if (m_isClosing.load()) {
		return AVERROR_EOF;
	}

	if (rtpQue.size() <= 0) {
		return AVERROR(EAGAIN);
	}

	SData &sd = rtpQue.front();
	int toRead = buf_size;
	if (toRead > sd.m_len) {
		toRead = sd.m_len;
	}

	memcpy(buf, sd.m_uBuf.get(), toRead);
	if (toRead < sd.m_len) {
		memmove(sd.m_uBuf.get(), sd.m_uBuf.get() + toRead, sd.m_len - toRead);
	}
	sd.m_len -= toRead;

	if (sd.m_len == 0) {
		pool.emplace(std::move(sd));
		rtpQue.pop();
	}

	return toRead;
}
