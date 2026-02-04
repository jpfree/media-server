#include "MsRtspSink.h"
#include "MsEvent.h"
#include "MsLog.h"
#include "MsResManager.h"
#include <thread>

class MsRtspHandler : public MsEventHandler {
public:
	MsRtspHandler(shared_ptr<MsReactor> reactor, shared_ptr<MsIRtspServer> iServer)
	    : m_reactor(reactor), m_iServer(iServer), m_bufSize(DEF_BUF_SIZE), m_bufOff(0) {
		m_bufPtr = make_unique<char[]>(m_bufSize);
	}

	~MsRtspHandler() { MS_LOG_INFO("~MsRtspHandler"); }

	void HandleRead(shared_ptr<MsEvent> evt) {
		MsSocket *sock = evt->GetSocket();

		int recv = sock->Recv(m_bufPtr.get() + m_bufOff, m_bufSize - 1 - m_bufOff);

		if (recv <= 0) {
			return;
		}

		m_bufOff += recv;
		m_bufPtr[m_bufOff] = '\0';

		this->ProcessBuffer(evt);
	}

	void ProcessBuffer(shared_ptr<MsEvent> evt) {
		char *p2 = m_bufPtr.get();

		while (m_bufOff > 0) {
			if (!IsHeaderComplete(p2)) {
				if (m_bufOff == m_bufSize - 1) {
					MS_LOG_ERROR("rtsp header too big:%d %s", m_bufOff, m_bufPtr.get());
					m_bufOff = 0;
				} else if (m_bufOff && p2 != m_bufPtr.get()) {
					memmove(m_bufPtr.get(), p2, m_bufOff);
				}

				return;
			}

			char *oriP2 = p2;
			MsRtspMsg rtspMsg;

			rtspMsg.Parse(p2);

			int cntLen = rtspMsg.m_contentLength.GetIntVal();
			int left = m_bufOff - (p2 - oriP2);
			int ret = 0;

			if (left < cntLen) {
				p2 = oriP2;

				if (m_bufOff && p2 != m_bufPtr.get()) {
					memmove(m_bufPtr.get(), p2, m_bufOff);
				}

				return;
			}

			MS_LOG_VERBS("recv:%s", m_bufPtr.get());

			if (rtspMsg.m_method == "OPTIONS") {
				ret = m_iServer->HandleOptions(rtspMsg, evt);
			} else if (rtspMsg.m_method == "DESCRIBE") {
				ret = m_iServer->HandleDescribe(rtspMsg, evt);
			} else if (rtspMsg.m_method == "SETUP") {
				ret = m_iServer->HandleSetup(rtspMsg, evt);
			} else if (rtspMsg.m_method == "PLAY") {
				ret = m_iServer->HandlePlay(rtspMsg, evt);
			} else if (rtspMsg.m_method == "TEARDOWN") {
				ret = m_iServer->HandleTeardown(rtspMsg, evt);
			} else if (rtspMsg.m_method == "PAUSE") {
				ret = m_iServer->HandlePause(rtspMsg, evt);
			} else if (rtspMsg.m_method == "SET_PARAMETER" || rtspMsg.m_method == "GET_PARAMETER") {
				ret = m_iServer->HandleOthers(rtspMsg, evt);
			} else {
				MS_LOG_ERROR("unknown method:%s", rtspMsg.m_method.c_str());
			}

			if (ret != 0) {
				MS_LOG_WARN("handle rtsp method:%s failed", rtspMsg.m_method.c_str());
				m_reactor->DelEvent(evt);
				return;
			}

			p2 += cntLen;
			m_bufOff -= (p2 - oriP2);
		}
	}

	void HandleClose(shared_ptr<MsEvent> evt) {
		m_reactor->DelEvent(evt);
		m_iServer->OnCloseEvent(evt);
	}

	void HandleWrite(shared_ptr<MsEvent> evt) { m_iServer->OnWriteEvent(evt); }

public:
	unique_ptr<char[]> m_bufPtr;
	int m_bufSize;
	int m_bufOff;

private:
	shared_ptr<MsReactor> m_reactor;
	shared_ptr<MsIRtspServer> m_iServer;
};

void MsRtspServer::Run() {
	this->RegistToManager();

	std::thread worker(&MsReactor::Wait, shared_from_this());
	worker.detach();
}

void MsRtspServer::HandleMsg(MsMsg &msg) {
	switch (msg.m_msgID) {
	case MS_SOCK_TRANSFER_MSG: {
		auto rtspMsg = std::any_cast<shared_ptr<SSockTransferMsg>>(msg.m_any);
		// Process rtspMsg as needed
		shared_ptr<MsRtspHandler> handler =
		    make_shared<MsRtspHandler>(dynamic_pointer_cast<MsRtspServer>(shared_from_this()),
		                               dynamic_pointer_cast<MsIRtspServer>(shared_from_this()));
		shared_ptr<MsEvent> event =
		    make_shared<MsEvent>(rtspMsg->sock, MS_FD_READ | MS_FD_CLOSE, handler);
		this->AddEvent(event);

		// copy data to handler buffer and process
		if (rtspMsg->data.size() > handler->m_bufSize) {
			handler->m_bufPtr = make_unique<char[]>(rtspMsg->data.size());
			handler->m_bufSize = rtspMsg->data.size();
		}

		memcpy(handler->m_bufPtr.get(), rtspMsg->data.data(), rtspMsg->data.size());
		handler->m_bufOff = rtspMsg->data.size();
		handler->ProcessBuffer(event);

	} break;
	default:
		MsReactor::HandleMsg(msg);
		break;
	}
}

int MsRtspServer::HandleOptions(MsRtspMsg &msg, shared_ptr<MsEvent> evt) {
	MsRtspMsg rsp;

	rsp.m_status = "200";
	rsp.m_reason = "OK";
	rsp.m_version = msg.m_version;
	rsp.m_cseq = msg.m_cseq;
	rsp.m_public.SetValue("OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, "
	                      "SET_PARAMETER, GET_PARAMETER");

	SendRtspMsg(rsp, evt->GetSocket());
	return 0;
}

int MsRtspServer::HandleDescribe(MsRtspMsg &msg, shared_ptr<MsEvent> evt) {
	MsRtspMsg rsp;
	rsp.m_version = msg.m_version;
	rsp.m_cseq = msg.m_cseq;

	size_t p = msg.m_uri.find("/", strlen("rtsp://"));
	if (p == string::npos) {
		MS_LOG_WARN("invalid uri:%s", msg.m_uri.c_str());
		rsp.m_status = "404";
		rsp.m_reason = "Not Found";
		SendRtspMsg(rsp, evt->GetSocket());
		return -1;
	}
	msg.m_uri = msg.m_uri.substr(p);

	std::vector<std::string> s = SplitString(msg.m_uri, "/");
	if (s.size() < 3) {
		MS_LOG_WARN("invalid uri:%s", msg.m_uri.c_str());
		rsp.m_status = "404";
		rsp.m_reason = "Not Found";
		SendRtspMsg(rsp, evt->GetSocket());
		return -1;
	}

	if (s[1] == "live") {
		std::string streamID = s[2];
		std::string format = "rtsp";

		std::shared_ptr<MsMediaSink> sink =
		    std::make_shared<MsRtspSink>(format, streamID, ++m_seqID, evt->GetSharedSocket(), msg);

		std::shared_ptr<MsMediaSource> source =
		    MsResManager::GetInstance().GetOrCreateMediaSource(s[1], streamID, "", sink);

		if (!source) {
			MS_LOG_WARN("create source failed for stream: %s", streamID.c_str());
			rsp.m_status = "404";
			rsp.m_reason = "Not Found";
			SendRtspMsg(rsp, evt->GetSocket());
			return -1;
		}

		this->DelEvent(evt);
		return 0;
	} else if (s[1] == "vod") {
		if (s.size() < 4) {
			MS_LOG_WARN("invalid vod uri:%s", msg.m_uri.c_str());
			rsp.m_status = "404";
			rsp.m_reason = "Not Found";
			SendRtspMsg(rsp, evt->GetSocket());
			return -1;
		}

		std::string streamID = s[2];
		std::string filename = s[3];
		std::string format = "rtsp";

		std::shared_ptr<MsMediaSink> sink =
		    std::make_shared<MsRtspSink>(format, streamID, ++m_seqID, evt->GetSharedSocket(), msg);

		// check media source exists
		auto source =
		    MsResManager::GetInstance().GetOrCreateMediaSource(s[1], streamID, filename, sink);

		if (!source) {
			MS_LOG_WARN("create VOD source failed for stream: %s", streamID.c_str());
			rsp.m_status = "404";
			rsp.m_reason = "Not Found";
			SendRtspMsg(rsp, evt->GetSocket());
			return -1;
		}

		this->DelEvent(evt);
		return 0;
	} else if (s[1] == "gbvod") {
		if (s.size() < 4) {
			MS_LOG_WARN("invalid gbvod uri:%s", msg.m_uri.c_str());
			rsp.m_status = "404";
			rsp.m_reason = "Not Found";
			SendRtspMsg(rsp, evt->GetSocket());
			return -1;
		}

		std::string streamID = s[2];
		std::string streamInfo = s[3];
		std::string format = "rtsp";

		std::shared_ptr<MsMediaSink> sink =
		    std::make_shared<MsRtspSink>(format, streamID, ++m_seqID, evt->GetSharedSocket(), msg);

		// check media source exists
		auto source =
		    MsResManager::GetInstance().GetOrCreateMediaSource(s[1], streamID, streamInfo, sink);

		if (!source) {
			MS_LOG_WARN("create GBVOD source failed for stream: %s", streamID.c_str());
			rsp.m_status = "404";
			rsp.m_reason = "Not Found";
			SendRtspMsg(rsp, evt->GetSocket());
			return -1;
		}

		this->DelEvent(evt);
		return 0;
	} else {
		MS_LOG_WARN("unsupported uri:%s", msg.m_uri.c_str());
		rsp.m_status = "404";
		rsp.m_reason = "Not Found";
		SendRtspMsg(rsp, evt->GetSocket());
		return -1;
	}
}

int MsRtspSink::HandleSetup(MsRtspMsg &msg, shared_ptr<MsEvent> evt) {
	MsRtspMsg rsp;
	rsp.m_version = msg.m_version;
	rsp.m_cseq = msg.m_cseq;
	size_t p = msg.m_transport.m_value.find("TCP");

	if (p == string::npos) {
		rsp.m_status = "461";
		rsp.m_reason = "Unsupported transport";
	} else {
		size_t p = msg.m_uri.find("streamid=0");
		int channel = 0;
		if (p == string::npos) {
			p = msg.m_uri.find("streamid=1");
			if (p == string::npos) {
				// error, no streamid
				rsp.m_status = "400";
				rsp.m_reason = "Bad Request";
				string data;
				rsp.Dump(data);
				this->WriteBuffer((const uint8_t *)data.c_str(), data.size(), -1);
				return 0;
			} else {
				channel = 1;
			}
		}

		char buf[64];
		snprintf(buf, sizeof(buf), "RTP/AVP/TCP;unicast;interleaved=%d-%d", channel * 2,
		         channel * 2 + 1);
		rsp.m_transport.SetValue(buf);
		rsp.m_status = "200";
		rsp.m_reason = "OK";

		if (msg.m_session.m_exist) {
			rsp.m_session = msg.m_session;
		} else {
			m_session = GenRandStr(16);
			rsp.m_session.SetValue(m_session);
		}
	}

	string data;
	rsp.Dump(data);
	this->WriteBuffer((const uint8_t *)data.c_str(), data.size(), -1);
	return 0;
}

int MsRtspSink::HandlePlay(MsRtspMsg &msg, shared_ptr<MsEvent> evt) {
	MsRtspMsg rsp;
	rsp.m_version = msg.m_version;
	rsp.m_cseq = msg.m_cseq;
	rsp.m_status = "200";
	rsp.m_reason = "OK";
	rsp.m_session = msg.m_session;
	string data;
	rsp.Dump(data);
	this->WriteBuffer((const uint8_t *)data.c_str(), data.size(), -1);

	if (!m_playing) {
		if (m_queData.size() > 0) {
			// regist write
			m_evt->SetEvent(MS_FD_READ | MS_FD_CLOSE | MS_FD_CONNECT);
			m_reactor->ModEvent(m_evt);

			MS_LOG_INFO("sink %s:%d regist write", m_type.c_str(), m_sinkID);
		}

		m_playing = true;
	}

	return 0;
}

int MsRtspSink::HandleTeardown(MsRtspMsg &msg, shared_ptr<MsEvent> evt) {
	MsRtspMsg rsp;
	rsp.m_version = msg.m_version;
	rsp.m_cseq = msg.m_cseq;
	rsp.m_status = "200";
	rsp.m_reason = "OK";
	rsp.m_session = msg.m_session;
	string data;
	rsp.Dump(data);

	this->WriteBuffer((const uint8_t *)data.c_str(), data.size(), -1);
	this->SinkActiveClose();
	return 0;
}

int MsRtspSink::HandleOthers(MsRtspMsg &msg, shared_ptr<MsEvent> evt) {
	MsRtspMsg rsp;
	string data;
	rsp.m_version = msg.m_version;
	rsp.m_cseq = msg.m_cseq;
	rsp.m_status = "200";
	rsp.m_reason = "OK";
	rsp.m_session = msg.m_session;
	rsp.Dump(data);
	this->WriteBuffer((const uint8_t *)data.c_str(), data.size(), -1);
	return 0;
}

int MsRtspSink::HandlePause(MsRtspMsg &msg, shared_ptr<MsEvent> evt) {
	// return not implemented
	MsRtspMsg rsp;
	string data;
	rsp.m_reason = "Not Implemented";
	rsp.m_status = "501";
	rsp.m_version = msg.m_version;
	rsp.m_cseq = msg.m_cseq;
	rsp.m_session = msg.m_session;
	rsp.Dump(data);
	this->WriteBuffer((const uint8_t *)data.c_str(), data.size(), -1);
	return 0;
}

void MsRtspSink::OnStreamInfo(AVStream *video, int videoIdx, AVStream *audio, int audioIdx) {
	if (m_error || !video)
		return;

	MsMediaSink::OnStreamInfo(video, videoIdx, audio, audioIdx);
	int buf_size = 2048;
	int ret;
	shared_ptr<MsEventHandler> handler;
	char sdp[2048];
	MsRtspMsg rsp;
	AVDictionary *opts = nullptr;

	m_reactor = MsReactorMgr::Instance()->GetReactor(MS_COMMON_REACTOR, 1);
	if (!m_reactor) {
		MS_LOG_ERROR("reactor not found for streamID:%s", m_streamID.c_str());
		goto err;
	}

	m_sock->SetNonBlock();
	handler = make_shared<MsRtspHandler>(m_reactor,
	                                     dynamic_pointer_cast<MsIRtspServer>(shared_from_this()));
	m_evt = std::make_shared<MsEvent>(m_sock, MS_FD_READ | MS_FD_CLOSE, handler);
	m_reactor->AddEvent(m_evt);

	m_pb = avio_alloc_context(
	    static_cast<unsigned char *>(av_malloc(buf_size)), buf_size, 1, this, nullptr,
	    [](void *opaque, IO_WRITE_BUF_TYPE *buf, int buf_size) -> int {
		    MsRtspSink *sink = static_cast<MsRtspSink *>(opaque);
		    return sink->WriteBuffer(buf, buf_size, 0);
	    },
	    nullptr);
	m_pb->max_packet_size = 1460;

	avformat_alloc_output_context2(&m_fmtCtx, nullptr, "rtp", nullptr);
	if (!m_fmtCtx || !m_pb) {
		MS_LOG_ERROR("Failed to allocate format context or IO context");
		goto err;
	}

	m_fmtCtx->pb = m_pb;
	m_fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

	m_outVideo = avformat_new_stream(m_fmtCtx, NULL);
	ret = avcodec_parameters_copy(m_outVideo->codecpar, m_video->codecpar);
	if (ret < 0) {
		MS_LOG_ERROR("Failed to copy codec parameters to output stream");
		goto err;
	}
	m_outVideo->codecpar->codec_tag = 0;

	av_dict_set(&opts, "rtpflags", "skip_rtcp", 0);
	if (m_outVideo->codecpar->codec_id == AV_CODEC_ID_AV1) {
		av_dict_set(&opts, "strict", "experimental", 0);
	}
	ret = avformat_write_header(m_fmtCtx, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		MS_LOG_ERROR("Error occurred when writing header to HTTP sink");
		goto err;
	}

	if (m_audio) {
		m_pbAudio = avio_alloc_context(
		    static_cast<unsigned char *>(av_malloc(buf_size)), buf_size, 1, this, nullptr,
		    [](void *opaque, IO_WRITE_BUF_TYPE *buf, int buf_size) -> int {
			    MsRtspSink *sink = static_cast<MsRtspSink *>(opaque);
			    return sink->WriteBuffer(buf, buf_size, 2);
		    },
		    nullptr);
		m_pbAudio->max_packet_size = 1460;

		avformat_alloc_output_context2(&m_fmtCtxAudio, nullptr, "rtp", nullptr);
		if (!m_fmtCtxAudio || !m_pbAudio) {
			MS_LOG_ERROR("Failed to allocate format context or IO context for audio");
			goto err;
		}

		m_fmtCtxAudio->pb = m_pbAudio;
		m_fmtCtxAudio->flags |= AVFMT_FLAG_CUSTOM_IO;

		m_outAudio = avformat_new_stream(m_fmtCtxAudio, NULL);
		ret = avcodec_parameters_copy(m_outAudio->codecpar, m_audio->codecpar);
		if (ret < 0) {
			MS_LOG_ERROR("Failed to copy codec parameters to output audio stream");
			goto err;
		}
		m_outAudio->codecpar->codec_tag = 0;

		AVCodecParameters *codecpar = m_outAudio->codecpar;
		// if no extradata, generate it
		if (codecpar->extradata_size == 0 && codecpar->sample_rate > 0 &&
		    codecpar->ch_layout.nb_channels > 0 && codecpar->ch_layout.nb_channels < 3 &&
		    codecpar->codec_id == AV_CODEC_ID_AAC) {
			// generate aac extradata
			// Map sample rate to index
			int samplingFrequencyIndex = 15; // default to escape value
			const static int sampleRateTable[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
			                                      22050, 16000, 12000, 11025, 8000,  7350};
			for (int i = 0; i < 13; i++) {
				if (sampleRateTable[i] == codecpar->sample_rate) {
					samplingFrequencyIndex = i;
					break;
				}
			}

			// Get audio object type (profile + 1, AAC-LC = 2)
			int audioObjectType =
			    (codecpar->profile > 0) ? codecpar->profile + 1 : 2; // Default to AAC-LC
			int channelConfiguration = codecpar->ch_layout.nb_channels;

			// Build Audio Specific Config (2 bytes for AAC-LC)
			// Byte 0: audioObjectType (5 bits) | samplingFrequencyIndex (3 bits)
			// Byte 1: samplingFrequencyIndex (1 bit) | channelConfiguration (4 bits) |
			// frameLengthFlag (1 bit) | dependsOnCoreCoder (1 bit)
			uint16_t config = 0;
			config |= (audioObjectType & 0x1F) << 11;       // 5 bits
			config |= (samplingFrequencyIndex & 0x0F) << 7; // 4 bits
			config |= (channelConfiguration & 0x0F) << 3;   // 4 bits
			// frameLengthFlag = 0 (1024 samples per frame), dependsOnCoreCoder = 0

			uint8_t extradata[2];
			extradata[0] = (config >> 8) & 0xFF;
			extradata[1] = config & 0xFF;
			int extradata_size = 2;

			codecpar->extradata =
			    (uint8_t *)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
			memcpy(codecpar->extradata, extradata, extradata_size);
			codecpar->extradata_size = extradata_size;
		}

		av_dict_set(&opts, "rtpflags", "skip_rtcp", 0);
		ret = avformat_write_header(m_fmtCtxAudio, &opts);
		av_dict_free(&opts);
		if (ret < 0) {
			MS_LOG_ERROR("Error occurred when writing header to rtsp sink for audio");
			goto err;
		}
	}

	{
		AVFormatContext *fmts[] = {m_fmtCtx, m_fmtCtxAudio};
		ret = av_sdp_create(fmts, m_fmtCtxAudio == nullptr ? 1 : 2, sdp, sizeof(sdp));
		if (ret < 0) {
			MS_LOG_ERROR("av_sdp_create failed");
			goto err;
		}
	}
	m_streamReady = true;

	rsp.m_version = m_descb.m_version;
	rsp.m_status = "200";
	rsp.m_reason = "OK";
	rsp.m_cseq = m_descb.m_cseq;
	rsp.m_contentType.SetValue("application/sdp");
	rsp.m_contentBase.SetValue(m_descb.m_uri);
	rsp.SetBody(sdp, strlen(sdp));
	SendRtspMsg(rsp, m_sock.get());

	return;

err:
	m_error = true;
	this->DetachSourceNoLock();
	this->SinkReleaseRes();
}

void MsRtspSink::OnSourceClose() {
	m_error = true;
	this->SinkReleaseRes();
}

void MsRtspSink::OnStreamPacket(AVPacket *pkt) {
	if (!m_streamReady || m_error) {
		return;
	}

	int ret;
	AVFormatContext *fmtCtx = pkt->stream_index == m_videoIdx ? m_fmtCtx : m_fmtCtxAudio;
	AVStream *outSt = pkt->stream_index == m_videoIdx ? m_outVideo : m_outAudio;
	AVStream *inSt = pkt->stream_index == m_videoIdx ? m_video : m_audio;
	int outIdx = pkt->stream_index == m_videoIdx ? m_outVideoIdx : m_outAudioIdx;
	int inIdx = pkt->stream_index;
	int64_t orig_pts = pkt->pts;
	int64_t orig_dts = pkt->dts;

	// drop non-key video frame at the beginning, how about audio?
	if (inIdx == m_videoIdx) {
		if (m_firstVideo) {
			if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
				return;
			}
		}
	} else {
		// buffer audio pkts
		if (m_firstVideo) {
			AVPacket *apkt = av_packet_clone(pkt);
			m_queAudioPkts.push(apkt);
			return;
		}
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

	if (inIdx == m_videoIdx) {
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
		MS_LOG_ERROR("Error writing frame to RTSP sink, ret:%d %s", ret, errbuf);
		MS_LOG_INFO("streamID:%s, sinkID:%d codec:%d pts:%ld dts:%ld size:%d", m_streamID.c_str(),
		            m_sinkID, inSt->codecpar->codec_id, pkt->pts, pkt->dts, pkt->size);
		MS_LOG_INFO("ori pts:%ld dts:%ld, in timebase %d/%d, out timebase %d/%d", orig_pts,
		            orig_dts, inSt->time_base.num, inSt->time_base.den, outSt->time_base.num,
		            outSt->time_base.den);
		MS_LOG_INFO("streamID:%s, sinkID:%d  1st vpts:%ld vdts:%ld apts:%ld adts:%ld",
		            m_streamID.c_str(), m_sinkID, m_firstVideoPts, m_firstVideoDts, m_firstAudioPts,
		            m_firstAudioDts);
	}

	while (m_queAudioPkts.size() && inIdx == m_videoIdx) {
		AVPacket *apkt = m_queAudioPkts.front();
		m_queAudioPkts.pop();
		int64_t ori_ms = orig_pts * 1000L * inSt->time_base.num / inSt->time_base.den;
		int64_t apkt_ms = apkt->pts * 1000L * m_audio->time_base.num / m_audio->time_base.den;
		int64_t diff = apkt_ms - ori_ms;

		if (diff > -131) { // allow max 131ms diff
			// send pkt
			this->OnStreamPacket(apkt);
		} else {
			// drop pkt
			MS_LOG_WARN("StreamID:%s, sinkID:%d drop buffered audio pkt, ori_ms:%ld "
			            "apkt_ms:%ld diff:%ld",
			            m_streamID.c_str(), m_sinkID, ori_ms, apkt_ms, diff);
		}
		av_packet_free(&apkt);
	}

	pkt->pts = orig_pts;
	pkt->dts = orig_dts;
	pkt->pos = -1;
	pkt->stream_index = inIdx;
}

void MsRtspSink::SinkActiveClose() {
	m_error = true;

	this->DetachSource();
	this->SinkReleaseRes();
}

void MsRtspSink::OnWriteEvent(shared_ptr<MsEvent> evt) {
	if (m_error)
		return;

	int ret = 0;
	auto sock = evt->GetSharedSocket();

	while (m_queData.size()) {
		SData &sd = m_queData.front();
		uint8_t *pBuf = sd.m_uBuf.get();

		while (sd.m_len > 0) {
			int psend = 0;
			ret = sock->Send((const char *)pBuf, sd.m_len, &psend);
			if (psend > 0) {
				sd.m_len -= psend;
				pBuf += psend;
			}

			if (ret >= 0) {
				// do nothing
			} else if (ret == MS_TRY_AGAIN) {
				if (pBuf != sd.m_uBuf.get()) {
					memmove(sd.m_uBuf.get(), pBuf, sd.m_len);
				}
				return;
			} else {
				MS_LOG_INFO("http sink streamID:%s, sinkID:%d err:%d", m_streamID.c_str(), m_sinkID,
				            MS_LAST_ERROR);
				this->SinkActiveClose();
				return;
			}
		}

		std::lock_guard<std::mutex> lock(m_queDataMutex);
		m_queData.pop();
	}

	// unregist write
	m_evt->SetEvent(MS_FD_READ | MS_FD_CLOSE);
	m_reactor->ModEvent(m_evt);

	MS_LOG_INFO("rtsp sink streamID:%s, sinkID:%d all data sent", m_streamID.c_str(), m_sinkID);
}

void MsRtspSink::OnCloseEvent(shared_ptr<MsEvent> evt) { this->SinkActiveClose(); }

// TODO: how to handle passive close gracefully, http stream too
int MsRtspSink::WriteBuffer(const uint8_t *buf, int buf_size, int channel) {
	if (m_error)
		return -1;

	if (channel == -1) {
		if (m_playing) {
			if (m_queData.size() > 0) {
				SData sd;
				sd.m_uBuf = std::make_unique<uint8_t[]>(buf_size);
				memcpy(sd.m_uBuf.get(), buf, buf_size);
				sd.m_len = buf_size;

				std::lock_guard<std::mutex> lock(m_queDataMutex);
				m_queData.emplace(std::move(sd));
				return buf_size;
			} else {
				m_sock->BlockSend((const char *)buf, buf_size);
				return buf_size;
			}
		} else {
			m_sock->BlockSend((const char *)buf, buf_size);
			return buf_size;
		}
	}

	if (!m_playing || m_queData.size()) {
		SData sd;
		sd.m_uBuf = std::make_unique<uint8_t[]>(buf_size + 4);
		uint8_t *pbuf = sd.m_uBuf.get();
		avio_w8(pbuf, '$');
		avio_w8(pbuf, channel);
		avio_wb16(pbuf, buf_size);
		memcpy(sd.m_uBuf.get() + 4, buf, buf_size);
		sd.m_len = buf_size + 4;

		std::lock_guard<std::mutex> lock(m_queDataMutex);
		m_queData.emplace(std::move(sd));
		return buf_size;
	} else {
		uint8_t header_buf[4];
		uint8_t *pheader = header_buf;
		avio_w8(pheader, '$');
		avio_w8(pheader, channel);
		avio_wb16(pheader, buf_size);

		if (m_sock->BlockSend((const char *)header_buf, 4) < 0) {
			MS_LOG_ERROR("rtsp sink send header failed");
			m_error = true;
			return buf_size;
		}

		const uint8_t *pBuf = buf;
		int pLen = buf_size;
		int ret = 0;

		while (pLen > 0) {
			int psend = 0;
			ret = m_sock->Send((const char *)pBuf, pLen, &psend);
			if (psend > 0) {
				pLen -= psend;
				pBuf += psend;
			}

			if (ret >= 0) {
				// do nothing
			} else if (ret == MS_TRY_AGAIN) {
				SData sd;
				sd.m_uBuf = std::make_unique<uint8_t[]>(pLen);
				memcpy(sd.m_uBuf.get(), pBuf, pLen);
				sd.m_len = pLen;
				{
					std::lock_guard<std::mutex> lock(m_queDataMutex);
					m_queData.emplace(std::move(sd));
				}

				// regist write
				m_evt->SetEvent(MS_FD_READ | MS_FD_CLOSE | MS_FD_CONNECT);
				m_reactor->ModEvent(m_evt);

				MS_LOG_INFO("sink %s:%d regist write", m_type.c_str(), m_sinkID);

				return buf_size;
			} else {
				MS_LOG_INFO("sink %s:%d err:%d", m_type.c_str(), m_sinkID, MS_LAST_ERROR);
				m_error = true;
				return buf_size;
			}
		}

		return buf_size;
	}
}

void MsRtspSink::SinkReleaseRes() {
	if (m_evt && m_reactor) {
		m_reactor->DelEvent(m_evt);
		m_evt = nullptr;
		m_reactor = nullptr;
	}

	if (m_fmtCtx) {
		av_write_trailer(m_fmtCtx);
		if (m_fmtCtx->pb) {
			avio_flush(m_fmtCtx->pb);
			av_freep(&m_fmtCtx->pb->buffer);
			avio_context_free(&m_fmtCtx->pb);
		}
		avformat_free_context(m_fmtCtx);
		m_fmtCtx = nullptr;
	}

	if (m_fmtCtxAudio) {
		av_write_trailer(m_fmtCtxAudio);
		if (m_fmtCtxAudio->pb) {
			avio_flush(m_fmtCtxAudio->pb);
			av_freep(&m_fmtCtxAudio->pb->buffer);
			avio_context_free(&m_fmtCtxAudio->pb);
		}
		avformat_free_context(m_fmtCtxAudio);
		m_fmtCtxAudio = nullptr;
	}

	m_outVideo = nullptr;
	m_outAudio = nullptr;

	while (m_queData.size()) {
		m_queData.pop();
	}

	while (m_queAudioPkts.size()) {
		AVPacket *pkt = m_queAudioPkts.front();
		m_queAudioPkts.pop();
		av_packet_free(&pkt);
	}
}
