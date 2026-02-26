#include "MsRtcSink.h"
#include "MsCommon.h"
#include "MsConfig.h"
#include "MsHttpMsg.h"
#include "MsLog.h"
#include "MsMsg.h"
#include "MsMsgDef.h"
#include "MsReactor.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

void MsRtcSink::SinkReleaseRes() {
	ReleaseTranscoder();

	if (m_videoFmtCtx) {
		if (m_videoFmtCtx->pb) {
			av_freep(&m_videoFmtCtx->pb->buffer);
			avio_context_free(&m_videoFmtCtx->pb);
		}
		avformat_free_context(m_videoFmtCtx);
		m_videoFmtCtx = nullptr;
	}

	if (m_audioFmtCtx) {
		if (m_audioFmtCtx->pb) {
			av_freep(&m_audioFmtCtx->pb->buffer);
			avio_context_free(&m_audioFmtCtx->pb);
		}
		avformat_free_context(m_audioFmtCtx);
		m_audioFmtCtx = nullptr;
	}

	if (_pc) {
		_pc->close();
		_pc.reset();
	}

	_videoTrack.reset();
	_audioTrack.reset();

	while (m_queAudioPkts.size() > 0) {
		AVPacket *pkt = m_queAudioPkts.front();
		av_packet_free(&pkt);
		m_queAudioPkts.pop();
	}

	while (m_queVideoPkts.size() > 0) {
		AVPacket *pkt = m_queVideoPkts.front();
		av_packet_free(&pkt);
		m_queVideoPkts.pop();
	}

	if (_onWhepPeerClosed) {
		_onWhepPeerClosed(_sessionId);
		_onWhepPeerClosed = nullptr;
	}
}

void MsRtcSink::SinkActiveClose() {
	m_error = true;

	this->DetachSource();
	this->SinkReleaseRes();
}

void MsRtcSink::OnSourceClose() {
	m_error = true;
	this->SinkReleaseRes();
}

void MsRtcSink::SetupWebRTC(const string &offerSdp, const string &httpVersion,
                            const string &location,
                            std::function<void(const string &)> onWhepPeerClosed) {
	_offerSdp = offerSdp;
	_httpVersion = httpVersion;
	_location = location;
	_onWhepPeerClosed = onWhepPeerClosed;
}

int MsRtcSink::CreateTracksAndAnswer() {
	if (m_error) {
		MS_LOG_WARN("whep:%s cannot create tracks, sink in error state", _sessionId.c_str());
		return -1;
	}

	rtc::Configuration config;
	config.enableIceTcp = true;
	config.enableIceUdpMux = true;
	config.portRangeBegin = MsConfig::Instance()->GetConfigInt("rtcPort");
	config.portRangeEnd = config.portRangeBegin;

	_pc = make_shared<rtc::PeerConnection>(config);

	rtc::Description offer(_offerSdp, "offer");
	for (int i = 0; i < offer.mediaCount(); ++i) {
		auto var = offer.media(i);
		auto media = std::get_if<rtc::Description::Media *>(&var);
		if (media == nullptr) {
			continue;
		}
		auto pMedia = *media;
		string type = pMedia->type();

		auto payloadTypes = pMedia->payloadTypes(); // Preload payload types
		for (auto &pt : payloadTypes) {
			rtc::Description::Media::RtpMap *rtpMap = nullptr;
			try {
				rtpMap = pMedia->rtpMap(pt);
			} catch (...) {
				continue;
			}

			if (rtpMap->format == "opus" && m_audio && !_audioTrack) {
				MS_LOG_INFO("whep:%s setup audio track, codec: %s, pt: %d", _sessionId.c_str(),
				            rtpMap->format.c_str(), pt);
				_audioPt = pt;
				_audioCodec = rtpMap->format;

				// Setup audio RTP muxer if audio exists
				if (m_audio) {
					// Initialize AAC to Opus transcoder if input is AAC
					if (m_audio->codecpar->codec_id == AV_CODEC_ID_AAC) {
						if (InitAacToOpusTranscoder() < 0) {
							MS_LOG_ERROR("Failed to initialize AAC to Opus transcoder");
							return -1;
						}
					}

					int buf_size = 2048;
					int ret;
					AVDictionary *opts = nullptr;
					char ptStr[8];

					m_audioPb = avio_alloc_context(
					    static_cast<unsigned char *>(av_malloc(buf_size)), buf_size, 1, this,
					    nullptr,
					    [](void *opaque, IO_WRITE_BUF_TYPE *buf, int buf_size) -> int {
						    MsRtcSink *sink = static_cast<MsRtcSink *>(opaque);
						    return sink->WriteBuffer(buf, buf_size, 0);
					    },
					    nullptr);
					m_audioPb->max_packet_size = 1200;

					avformat_alloc_output_context2(&m_audioFmtCtx, nullptr, "rtp", nullptr);
					if (!m_audioFmtCtx || !m_audioPb) {
						MS_LOG_ERROR("Failed to allocate audio format context or IO context");
						return -1;
					}

					m_audioFmtCtx->pb = m_audioPb;
					m_audioFmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

					m_outAudio = avformat_new_stream(m_audioFmtCtx, NULL);

					// If transcoding, use Opus parameters; otherwise copy from input
					if (m_needTranscode && m_opusEncCtx) {
						ret = avcodec_parameters_from_context(m_outAudio->codecpar, m_opusEncCtx);
						if (ret < 0) {
							MS_LOG_ERROR("Failed to copy Opus codec parameters");
							return -1;
						}
						m_outAudio->time_base = m_opusEncCtx->time_base;
					} else {
						ret = avcodec_parameters_copy(m_outAudio->codecpar, m_audio->codecpar);
						if (ret < 0) {
							MS_LOG_ERROR("Failed to copy audio codec parameters");
							return -1;
						}
					}
					m_outAudio->codecpar->codec_tag = 0;

					// Set RTP payload type for audio
					snprintf(ptStr, sizeof(ptStr), "%d", _audioPt);
					av_dict_set(&opts, "payload_type", ptStr, 0);
					av_dict_set(&opts, "rtpflags", "skip_rtcp", 0);
					ret = avformat_write_header(m_audioFmtCtx, &opts);
					av_dict_free(&opts);
					if (ret < 0) {
						MS_LOG_ERROR("Error writing audio RTP header");
						return -1;
					}
				}

				rtc::Description::Audio audioDesc(pMedia->mid());
				audioDesc.addOpusCodec(pt);
				auto ssrcs = pMedia->getSSRCs();
				for (auto ssrc : ssrcs) {
					audioDesc.addSSRC(ssrc, pMedia->getCNameForSsrc(ssrc));
				}
				_audioTrack = _pc->addTrack(std::move(audioDesc));
			} else if (IsVideoMatch(rtpMap->format, m_video->codecpar->codec_id) && !_videoTrack) {
				MS_LOG_INFO("whep:%s setup video track, codec: %s, pt: %d", _sessionId.c_str(),
				            rtpMap->format.c_str(), pt);

				_videoPt = pt;
				_videoCodec = rtpMap->format;

				int buf_size = 2048;
				int ret;
				AVDictionary *opts = nullptr;
				char ptStr[8];

				// Setup video RTP muxer
				m_videoPb = avio_alloc_context(
				    static_cast<unsigned char *>(av_malloc(buf_size)), buf_size, 1, this, nullptr,
				    [](void *opaque, IO_WRITE_BUF_TYPE *buf, int buf_size) -> int {
					    MsRtcSink *sink = static_cast<MsRtcSink *>(opaque);
					    return sink->WriteBuffer(buf, buf_size, 1);
				    },
				    nullptr);
				m_videoPb->max_packet_size = 1200; // MTU for WebRTC

				avformat_alloc_output_context2(&m_videoFmtCtx, nullptr, "rtp", nullptr);
				if (!m_videoFmtCtx || !m_videoPb) {
					MS_LOG_ERROR("Failed to allocate video format context or IO context");
					return -1;
				}

				m_videoFmtCtx->pb = m_videoPb;
				m_videoFmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

				m_outVideo = avformat_new_stream(m_videoFmtCtx, NULL);
				ret = avcodec_parameters_copy(m_outVideo->codecpar, m_video->codecpar);
				if (ret < 0) {
					MS_LOG_ERROR("Failed to copy video codec parameters");
					return -1;
				}
				m_outVideo->codecpar->codec_tag = 0;

				// Set RTP payload type for video
				snprintf(ptStr, sizeof(ptStr), "%d", _videoPt);
				av_dict_set(&opts, "payload_type", ptStr, 0);
				av_dict_set(&opts, "rtpflags", "skip_rtcp", 0);

				if (_videoCodec == "VP9" || _videoCodec == "AV1") {
					av_dict_set(&opts, "strict", "experimental", 0);
				}

				ret = avformat_write_header(m_videoFmtCtx, &opts);
				av_dict_free(&opts);
				if (ret < 0) {
					MS_LOG_ERROR("Error writing video RTP header");
					return -1;
				}

				rtc::Description::Video videoDesc(pMedia->mid());

				if (rtpMap->format == "H264") {
					videoDesc.addH264Codec(pt);
				} else if (rtpMap->format == "H265") {
					videoDesc.addH265Codec(pt);
				} else if (rtpMap->format == "AV1") {
					videoDesc.addAV1Codec(pt);
				} else if (rtpMap->format == "VP8") {
					videoDesc.addVP8Codec(pt);
				} else if (rtpMap->format == "VP9") {
					videoDesc.addVP9Codec(pt);
				}

				auto ssrcs = pMedia->getSSRCs();
				for (auto ssrc : ssrcs) {
					videoDesc.addSSRC(ssrc, pMedia->getCNameForSsrc(ssrc));
				}
				_videoTrack = _pc->addTrack(std::move(videoDesc));
			}
		}
	}

	// Setup state change callback
	string sessionId = _sessionId;
	auto onPeerClosed = _onWhepPeerClosed;
	_pc->onStateChange([this, sessionId, onPeerClosed](rtc::PeerConnection::State state) {
		string strState;
		switch (state) {
		case rtc::PeerConnection::State::New:
			strState = "New";
			break;
		case rtc::PeerConnection::State::Connecting:
			strState = "Connecting";
			break;
		case rtc::PeerConnection::State::Connected:
			strState = "Connected";
			m_streamReady = true;
			break;
		case rtc::PeerConnection::State::Disconnected:
			strState = "Disconnected";
			break;
		case rtc::PeerConnection::State::Failed:
			strState = "Failed";
			break;
		case rtc::PeerConnection::State::Closed:
			strState = "Closed";
			if (onPeerClosed) {
				onPeerClosed(sessionId);
			}
			break;
		}
		MS_LOG_INFO("whep:%s state:%s", sessionId.c_str(), strState.c_str());
	});

	_pc->onTrack([this, sessionId](shared_ptr<rtc::Track> track) {
		weak_ptr<rtc::Track> weak_track = track;
		track->onOpen([weak_track, sessionId]() {
			auto track = weak_track.lock();
			if (track)
				MS_LOG_INFO("pc:%s track open:%s", sessionId.c_str(), track->mid().c_str());
		});

		std::shared_ptr<rtc::RtcpReceivingSession> session =
		    std::make_shared<rtc::RtcpReceivingSession>();
		track->setMediaHandler(session);
	});

	_pc->onLocalDescription([](rtc::Description description) {});

	std::weak_ptr<rtc::PeerConnection> weak_pc = _pc;
	std::weak_ptr<MsSocket> weak_sock = _sock;
	string version = _httpVersion;
	string location = _location;

	_pc->onGatheringStateChange([this, weak_pc, weak_sock, sessionId, version,
	                             location](rtc::PeerConnection::GatheringState state) {
		if (state == rtc::PeerConnection::GatheringState::Complete) {
			auto pc = weak_pc.lock();
			if (!pc)
				return;

			auto sock = weak_sock.lock();
			if (!sock)
				return;

			auto description = pc->localDescription();
			string sdp = description->generateSdp();

			MsHttpMsg rsp;
			rsp.m_version = version;

			if (_videoPt == 0 && _audioPt == 0) {
				MS_LOG_ERROR("whep:%s no valid tracks to create answer", sessionId.c_str());
				rsp.m_status = "500";
				rsp.m_reason = "Internal Server Error";
			} else {
				rsp.m_status = "201";
				rsp.m_reason = "Created";
				rsp.m_location.SetValue(location);
				rsp.m_contentType.SetValue("application/sdp");
				rsp.m_exposeHeader.SetValue("Location");
				rsp.SetBody(sdp.c_str(), sdp.size());
			}

			SendHttpRspEx(sock.get(), rsp);

			MS_LOG_INFO("whep:%s SDP answer sent", sessionId.c_str());

			// should send MS_RTC_DEL_SOCK message to MsRtcServer to delete socket
			MsMsg msg;
			msg.m_msgID = MS_RTC_DEL_SOCK;
			msg.m_strVal = sessionId;
			msg.m_dstType = MS_RTC_SERVER;
			msg.m_dstID = 1;
			MsReactorMgr::Instance()->PostMsg(msg);
		}
	});

	// Parse and set remote description
	try {
		_pc->setRemoteDescription(std::move(offer));
	} catch (const std::exception &e) {
		MS_LOG_ERROR("whep:%s setRemoteDescription exception:%s", _sessionId.c_str(), e.what());
		m_error = true;

		MsHttpMsg rsp;
		rsp.m_version = _httpVersion;
		rsp.m_status = "500";
		rsp.m_reason = "Internal Server Error";
		if (_sock) {
			SendHttpRspEx(_sock.get(), rsp);
		}

		return -1;
	}

	return 0;
}

void MsRtcSink::OnStreamInfo(AVStream *video, int videoIdx, AVStream *audio, int audioIdx) {
	if (m_error || !video)
		return;

	MsMediaSink::OnStreamInfo(video, videoIdx, audio, audioIdx);

	// Now create WebRTC tracks based on actual stream info and send SDP answer
	if (CreateTracksAndAnswer() < 0) {
		goto err;
	}

	return;

err:
	m_error = true;
	this->DetachSourceNoLock();
	this->SinkReleaseRes();
}

void MsRtcSink::OnStreamPacket(AVPacket *pkt) {
	if (!m_streamReady || m_error) {
		if (m_error) {
			MS_LOG_WARN("whep:%s cannot process packet, sink in error state", _sessionId.c_str());
			return;
		}

		if (pkt->stream_index == m_videoIdx) {
			m_queVideoPkts.push(av_packet_clone(pkt));
		} else {
			m_queAudioPkts.push(av_packet_clone(pkt));
		}

		return;
	}

	while (m_queVideoPkts.size() > 0) {
		AVPacket *vpk = m_queVideoPkts.front();
		m_queVideoPkts.pop();
		this->ProcessPkt(vpk);
		av_packet_free(&vpk);
	}

	this->ProcessPkt(pkt);
}

int MsRtcSink::InitAacToOpusTranscoder() {
	if (!m_audio || m_audio->codecpar->codec_id != AV_CODEC_ID_AAC) {
		return 0; // No transcoding needed
	}

	m_needTranscode = true;
	int ret;

	// Initialize AAC decoder
	const AVCodec *aacDecoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (!aacDecoder) {
		MS_LOG_ERROR("whep:%s AAC decoder not found", _sessionId.c_str());
		return -1;
	}

	m_aacDecCtx = avcodec_alloc_context3(aacDecoder);
	if (!m_aacDecCtx) {
		MS_LOG_ERROR("whep:%s failed to allocate AAC decoder context", _sessionId.c_str());
		return -1;
	}

	ret = avcodec_parameters_to_context(m_aacDecCtx, m_audio->codecpar);
	if (ret < 0) {
		MS_LOG_ERROR("whep:%s failed to copy AAC codec parameters", _sessionId.c_str());
		return -1;
	}

	ret = avcodec_open2(m_aacDecCtx, aacDecoder, nullptr);
	if (ret < 0) {
		MS_LOG_ERROR("whep:%s failed to open AAC decoder", _sessionId.c_str());
		return -1;
	}

	// Initialize Opus encoder
	const AVCodec *opusEncoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
	if (!opusEncoder) {
		MS_LOG_ERROR("whep:%s Opus encoder not found", _sessionId.c_str());
		return -1;
	}

	m_opusEncCtx = avcodec_alloc_context3(opusEncoder);
	if (!m_opusEncCtx) {
		MS_LOG_ERROR("whep:%s failed to allocate Opus encoder context", _sessionId.c_str());
		return -1;
	}

	// Configure Opus encoder - WebRTC typically uses 48kHz stereo
	m_opusEncCtx->sample_rate = 48000;
	m_opusEncCtx->bit_rate = 64000;
	m_opusEncCtx->sample_fmt = AV_SAMPLE_FMT_FLT;
	m_opusEncCtx->time_base = (AVRational){1, 48000};

#if LIBAVUTIL_VERSION_MAJOR >= 58
	AVChannelLayout chLayout = AV_CHANNEL_LAYOUT_STEREO;
	av_channel_layout_copy(&m_opusEncCtx->ch_layout, &chLayout);
#else
	m_opusEncCtx->channels = 2;
	m_opusEncCtx->channel_layout = AV_CH_LAYOUT_STEREO;
#endif

	// Low latency settings for Opus
	// av_opt_set(m_opusEncCtx->priv_data, "application", "lowdelay", 0);
	// // av_opt_set_int(m_opusEncCtx->priv_data, "packet_loss", 0, 0);
	// av_opt_set_int(m_opusEncCtx->priv_data, "frame_duration", 20, 0); // 20ms frames

	ret = avcodec_open2(m_opusEncCtx, opusEncoder, nullptr);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_strerror(ret, errbuf, sizeof(errbuf));
		MS_LOG_ERROR("whep:%s failed to open Opus encoder: %s", _sessionId.c_str(), errbuf);
		return -1;
	}

	m_swrCtx = swr_alloc();
	av_opt_set_chlayout(m_swrCtx, "in_chlayout", &m_aacDecCtx->ch_layout, 0);
	av_opt_set_int(m_swrCtx, "in_sample_rate", m_aacDecCtx->sample_rate, 0);
	av_opt_set_sample_fmt(m_swrCtx, "in_sample_fmt", m_aacDecCtx->sample_fmt, 0);

	av_opt_set_chlayout(m_swrCtx, "out_chlayout", &m_opusEncCtx->ch_layout, 0);
	av_opt_set_int(m_swrCtx, "out_sample_rate", m_opusEncCtx->sample_rate, 0);
	av_opt_set_sample_fmt(m_swrCtx, "out_sample_fmt", m_opusEncCtx->sample_fmt, 0);

	ret = swr_init(m_swrCtx);
	if (ret < 0) {
		MS_LOG_ERROR("whep:%s failed to initialize SwrContext", _sessionId.c_str());
		return -1;
	}

	// FIFO to buffer samples until we have enough for one Opus frame
	m_audioFifo =
	    av_audio_fifo_alloc(m_opusEncCtx->sample_fmt, m_opusEncCtx->ch_layout.nb_channels, 4096);

	// Allocate frames
	m_decodedFrame = av_frame_alloc();
	m_opusFrame = av_frame_alloc();

	// Pre-allocate Opus frame buffer
	m_opusFrame->nb_samples = m_opusEncCtx->frame_size;
	m_opusFrame->format = m_opusEncCtx->sample_fmt;
	m_opusFrame->ch_layout = m_opusEncCtx->ch_layout;
	m_opusFrame->sample_rate = m_opusEncCtx->sample_rate;

	ret = av_frame_get_buffer(m_opusFrame, 0);
	if (ret < 0) {
		MS_LOG_ERROR("whep:%s failed to allocate resampled frame buffer", _sessionId.c_str());
		return -1;
	}

	m_nextOpusPts = 0;
	MS_LOG_INFO("whep:%s AAC to Opus transcoder initialized, AAC %dHz -> Opus %dHz",
	            _sessionId.c_str(), m_aacDecCtx->sample_rate, m_opusEncCtx->sample_rate);

	return 0;
}

void MsRtcSink::TranscodeAAC(AVPacket *pkt, std::vector<AVPacket *> &opusPkts) {
	if (!m_aacDecCtx || !m_opusEncCtx || !m_swrCtx) {
		MS_LOG_WARN("whep:%s transcoder not initialized", _sessionId.c_str());
		return;
	}

	int ret;

	ret = avcodec_send_packet(m_aacDecCtx, pkt);
	if (ret < 0) {
		MS_LOG_DEBUG("whep:%s error:%d sending AAC packet to decoder", _sessionId.c_str(), ret);
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			MS_LOG_WARN("whep:%s error sending AAC packet to decoder", _sessionId.c_str());
		}
		return;
	}

	// Receive decoded frame
	ret = avcodec_receive_frame(m_aacDecCtx, m_decodedFrame);
	if (ret < 0) {
		MS_LOG_DEBUG("whep:%s error:%d receiving decoded frame", _sessionId.c_str(), ret);
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			MS_LOG_WARN("whep:%s error receiving decoded frame", _sessionId.c_str());
		}
		return;
	}

	int dst_nb_samples = av_rescale_rnd(
	    swr_get_delay(m_swrCtx, m_aacDecCtx->sample_rate) + m_decodedFrame->nb_samples,
	    m_opusEncCtx->sample_rate, m_aacDecCtx->sample_rate, AV_ROUND_UP);

	uint8_t **converted_data = nullptr;
	int linesize;
	av_samples_alloc_array_and_samples(&converted_data, &linesize,
	                                   m_opusEncCtx->ch_layout.nb_channels, dst_nb_samples,
	                                   m_opusEncCtx->sample_fmt, 0);

	// Convert
	int converted_count =
	    swr_convert(m_swrCtx, converted_data, dst_nb_samples,
	                (const uint8_t **)m_decodedFrame->data, m_decodedFrame->nb_samples);

	// C. Write to FIFO
	av_audio_fifo_write(m_audioFifo, (void **)converted_data, converted_count);

	if (converted_data) {
		av_freep(&converted_data[0]);
		av_freep(&converted_data);
	}

	// D. Encode from FIFO
	// Opus frame size is usually 960 samples (20ms at 48kHz)
	while (av_audio_fifo_size(m_audioFifo) >= m_opusEncCtx->frame_size) {
		// Pull data from FIFO
		if (av_frame_make_writable(m_opusFrame) < 0) {
			MS_LOG_WARN("whep:%s Opus frame not writable", _sessionId.c_str());
			break;
		}
		av_audio_fifo_read(m_audioFifo, (void **)m_opusFrame->data, m_opusEncCtx->frame_size);

		// Set PTS
		m_opusFrame->pts = m_nextOpusPts;
		m_nextOpusPts += m_opusFrame->nb_samples;
		// Send to Encoder
		if (avcodec_send_frame(m_opusEncCtx, m_opusFrame) == 0) {
			AVPacket *out_pkt = av_packet_alloc();
			while (avcodec_receive_packet(m_opusEncCtx, out_pkt) == 0) {
				opusPkts.push_back(out_pkt);
				out_pkt = av_packet_alloc();
			}
			av_packet_free(&out_pkt);
		}
	}

	av_frame_unref(m_decodedFrame);
}

bool MsRtcSink::IsVideoMatch(const string &codec, AVCodecID codec_id) {
	if ((codec == "H264" && codec_id == AV_CODEC_ID_H264) ||
	    (codec == "H265" && codec_id == AV_CODEC_ID_H265) ||
	    (codec == "AV1" && codec_id == AV_CODEC_ID_AV1)) {
		return true;
	}
	return false;
}

void MsRtcSink::ReleaseTranscoder() {
	if (m_aacDecCtx) {
		avcodec_free_context(&m_aacDecCtx);
		m_aacDecCtx = nullptr;
	}
	if (m_opusEncCtx) {
		// Flush the opus encoder before closing
		avcodec_send_frame(m_opusEncCtx, nullptr);
		AVPacket *flush_pkt = av_packet_alloc();
		while (flush_pkt && avcodec_receive_packet(m_opusEncCtx, flush_pkt) == 0) {
			// Drop all remaining audio frames
			av_packet_unref(flush_pkt);
		}
		av_packet_free(&flush_pkt);

		avcodec_free_context(&m_opusEncCtx);
		m_opusEncCtx = nullptr;
	}
	if (m_swrCtx) {
		swr_free(&m_swrCtx);
		m_swrCtx = nullptr;
	}
	if (m_decodedFrame) {
		av_frame_free(&m_decodedFrame);
		m_decodedFrame = nullptr;
	}
	if (m_opusFrame) {
		av_frame_free(&m_opusFrame);
		m_opusFrame = nullptr;
	}

	if (m_audioFifo) {
		av_audio_fifo_free(m_audioFifo);
		m_audioFifo = nullptr;
	}
}

void MsRtcSink::ProcessPkt(AVPacket *pkt) {
	int ret;
	bool isVideo = pkt->stream_index == m_videoIdx;
	AVFormatContext *fmtCtx = isVideo ? m_videoFmtCtx : m_audioFmtCtx;
	AVStream *outSt = isVideo ? m_outVideo : m_outAudio;
	AVStream *inSt = isVideo ? m_video : m_audio;
	int outIdx = isVideo ? m_outVideoIdx : m_outAudioIdx;
	int inIdx = pkt->stream_index;
	int64_t orig_pts = pkt->pts;
	int64_t orig_dts = pkt->dts;

	if (!fmtCtx) {
		return;
	}

	// Drop non-key video frame at the beginning
	if (isVideo && m_firstVideo) {
		if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
			// MS_LOG_WARN("StreamID:%s, sinkID:%d drop non-key video pkt at the beginning "
			//             "pts:%ld dts:%ld size:%d",
			//             m_streamID.c_str(), m_sinkID, pkt->pts, pkt->dts, pkt->size);
			return;
		}
	} else if (!isVideo && m_firstVideo) {
		// buffer audio pkts
		AVPacket *apkt = av_packet_clone(pkt);
		m_queAudioPkts.push(apkt);
		return;
	}

	// Handle audio transcoding (AAC to Opus)
	if (!isVideo && m_needTranscode) {
		std::vector<AVPacket *> opusPkts;
		this->TranscodeAAC(pkt, opusPkts);

		for (auto opusPkt : opusPkts) {
			opusPkt->stream_index = outIdx;
			opusPkt->pos = -1;

			if (m_firstAudio) {
				m_firstAudio = false;
				m_firstAudioPts = opusPkt->pts;
				m_firstAudioDts = opusPkt->dts;
			}

			opusPkt->pts -= m_firstAudioPts;
			if (opusPkt->dts != AV_NOPTS_VALUE && m_firstAudioDts != AV_NOPTS_VALUE) {
				opusPkt->dts -= m_firstAudioDts;
			}

			ret = av_write_frame(fmtCtx, opusPkt);
			if (ret < 0) {
				char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
				av_strerror(ret, errbuf, sizeof(errbuf));
				MS_LOG_ERROR("Error writing transcoded audio frame to RTC sink, ret:%d %s", ret,
				             errbuf);
			}

			av_packet_free(&opusPkt);
		}

		pkt->pts = orig_pts;
		pkt->dts = orig_dts;
		pkt->pos = -1;
		pkt->stream_index = inIdx;

		return;
	}

	av_packet_rescale_ts(pkt, inSt->time_base, outSt->time_base);
	pkt->pos = -1;
	pkt->stream_index = outIdx;

	if (pkt->pts == AV_NOPTS_VALUE) {
		MS_LOG_WARN("pkt pts is AV_NOPTS_VALUE, streamID:%s, sinkID:%d codec:%d "
		            "pts:%ld dts:%ld size:%d",
		            m_streamID.c_str(), m_sinkID, inSt->codecpar->codec_id, pkt->pts, pkt->dts,
		            pkt->size);
		MS_LOG_INFO("ori pts:%ld dts:%ld, in timebase %d/%d, out timebase %d/%d", orig_pts,
		            orig_dts, inSt->time_base.num, inSt->time_base.den, outSt->time_base.num,
		            outSt->time_base.den);
	}

	if (isVideo) {
		if (m_firstVideo) {
			m_firstVideo = false;
			m_firstVideoPts = pkt->pts;
			m_firstVideoDts = pkt->dts;
		}
		pkt->pts -= m_firstVideoPts;
		if (pkt->dts != AV_NOPTS_VALUE && m_firstVideoDts != AV_NOPTS_VALUE) {
			pkt->dts -= m_firstVideoDts;
		}
	} else {
		if (m_firstAudio) {
			m_firstAudio = false;
			m_firstAudioPts = pkt->pts;
			m_firstAudioDts = pkt->dts;
		}
		pkt->pts -= m_firstAudioPts;
		if (pkt->dts != AV_NOPTS_VALUE && m_firstAudioDts != AV_NOPTS_VALUE) {
			pkt->dts -= m_firstAudioDts;
		}
	}

	ret = av_write_frame(fmtCtx, pkt);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_strerror(ret, errbuf, sizeof(errbuf));
		MS_LOG_ERROR("Error writing frame to RTC sink, ret:%d %s", ret, errbuf);
	}

	while (m_queAudioPkts.size() && isVideo) {
		AVPacket *apkt = m_queAudioPkts.front();
		m_queAudioPkts.pop();
		int64_t ori_ms = orig_pts * 1000L * inSt->time_base.num / inSt->time_base.den;
		int64_t apkt_ms = apkt->pts * 1000L * m_audio->time_base.num / m_audio->time_base.den;
		int64_t diff = apkt_ms - ori_ms;

		if (diff > -131) { // allow max 131ms diff
			// send pkt
			this->ProcessPkt(apkt);
		} else {
			// // drop pkt
			MS_LOG_WARN("StreamID:%s, sinkID:%d drop buffered audio pkt, ori_ms:%ld "
			            "apkt_ms:%ld diff:%ld",
			            m_streamID.c_str(), m_sinkID, ori_ms, apkt_ms, diff);
		}
		av_packet_free(&apkt);
	}

	// Restore original values
	pkt->pts = orig_pts;
	pkt->dts = orig_dts;
	pkt->pos = -1;
	pkt->stream_index = inIdx;
}

int MsRtcSink::WriteBuffer(const uint8_t *buf, int buf_size, int8_t isVideo) {
	if (m_error || buf_size < 12) {
		return -1;
	}

	// Send RTP packet via WebRTC track
	auto track = isVideo ? _videoTrack : _audioTrack;
	if (track && track->isOpen()) {
		try {
			rtc::binary data(reinterpret_cast<const std::byte *>(buf),
			                 reinterpret_cast<const std::byte *>(buf + buf_size));
			track->send(data);
		} catch (const std::exception &e) {
			MS_LOG_WARN("Failed to send RTP packet: %s", e.what());
			return -1;
		}
	}

	return buf_size;
}
