#include "MsHttpSink.h"

void MsHttpSink::HandleRead(shared_ptr<MsEvent> evt) {
	char buf[512];
	int ret = m_sock->Recv(buf, 512);
	if (ret == 0) {
		this->SinkActiveClose();
	}
}

void MsHttpSink::HandleClose(shared_ptr<MsEvent> evt) {
	MS_LOG_ERROR("http sink socket closed, streamID:%s, sinkID:%d", m_streamID.c_str(), m_sinkID);
	this->SinkActiveClose();
}

void MsHttpSink::HandleWrite(shared_ptr<MsEvent> evt) {
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
				// do nothing, handled in psend
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

	MS_LOG_INFO("http sink streamID:%s, sinkID:%d all data sent", m_streamID.c_str(), m_sinkID);
}

// TODO: add proper time duration handling
// TODO: gb record play back, ts has aac timestamp gap issue when
//       the audio pkt has dup timestamp, flv muxer can handle it,
//       but ts muxer not, need further investigate
void MsHttpSink::OnStreamInfo(AVStream *video, int videoIdx, AVStream *audio, int audioIdx) {
	if (m_error || !video)
		return;
	MsMediaSink::OnStreamInfo(video, videoIdx, audio, audioIdx);
	int buf_size = 2048;
	int ret;

	m_reactor = MsReactorMgr::Instance()->GetReactor(MS_COMMON_REACTOR, 1);
	if (!m_reactor) {
		MS_LOG_ERROR("reactor not found for streamID:%s", m_streamID.c_str());
		goto err;
	}

	m_sock->SetNonBlock();
	m_evt = std::make_shared<MsEvent>(m_sock, MS_FD_READ | MS_FD_CLOSE, shared_from_this());
	m_reactor->AddEvent(m_evt);

	m_pb = avio_alloc_context(
	    static_cast<unsigned char *>(av_malloc(buf_size)), buf_size, 1, this, nullptr,
	    [](void *opaque, IO_WRITE_BUF_TYPE *buf, int buf_size) -> int {
		    MsHttpSink *sink = static_cast<MsHttpSink *>(opaque);
		    return sink->WriteBuffer(buf, buf_size);
	    },
	    nullptr);

	if (m_type == "flv")
		avformat_alloc_output_context2(&m_fmtCtx, nullptr, "flv", nullptr);
	else if (m_type == "ts")
		avformat_alloc_output_context2(&m_fmtCtx, nullptr, "mpegts", nullptr);
	else
		avformat_alloc_output_context2(&m_fmtCtx, nullptr, m_type.c_str(), nullptr);
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

	if (m_video->duration > 0) {
		m_fmtCtx->duration =
		    static_cast<int64_t>(m_video->duration * av_q2d(m_video->time_base) * AV_TIME_BASE);
	}

	if (m_audio) {
		m_outAudio = avformat_new_stream(m_fmtCtx, NULL);
		ret = avcodec_parameters_copy(m_outAudio->codecpar, m_audio->codecpar);
		if (ret < 0) {
			MS_LOG_ERROR("Failed to copy codec parameters to output stream");
			goto err;
		}
		m_outAudio->codecpar->codec_tag = 0;
	}

	ret = avformat_write_header(m_fmtCtx, NULL);
	if (ret < 0) {
		MS_LOG_ERROR("Error occurred when writing header to HTTP sink");
		goto err;
	}

	m_streamReady = true;
	return;

err:
	m_error = true;
	this->DetachSourceNoLock();
	this->SinkReleaseRes();
}

int MsHttpSink::WriteBuffer(const uint8_t *buf, int buf_size) {
	if (m_error)
		return -1;

	if (m_firstPacket) {
		m_firstPacket = false;
		MsHttpMsg rsp;

		rsp.m_version = "HTTP/1.1";
		rsp.m_status = "200";
		rsp.m_reason = "OK";
		rsp.m_connection.SetValue("close");
		if (m_type == "flv") {
			rsp.m_contentType.SetValue("video/x-flv");
		} else if (m_type == "ts") {
			rsp.m_contentType.SetValue("video/MP2T");
		} else {
			rsp.m_contentType.SetValue("application/octet-stream");
		}

		rsp.m_transport.SetValue("chunked");
		rsp.m_allowOrigin.SetValue("*");
		rsp.m_allowMethod.SetValue("GET, POST, OPTIONS");
		rsp.m_allowHeader.SetValue(
		    "DNT,X-Mx-ReqToken,range,Keep-Alive,User-Agent,X-Requested-With,If-"
		    "Modified-Since,Cache-Control,Content-Type,Authorization");

		SendHttpRsp(m_sock.get(), rsp);
	}

	// Format chunk size in hex
	char chunk_header[32];
	int header_len = snprintf(chunk_header, sizeof(chunk_header), "%x\r\n", buf_size);
	int total_chunk_size = header_len + buf_size + 2; // +2 for \r\n after data

	if (m_queData.size()) {
		SData sd;
		sd.m_uBuf = std::make_unique<uint8_t[]>(total_chunk_size);
		memcpy(sd.m_uBuf.get(), chunk_header, header_len);
		memcpy(sd.m_uBuf.get() + header_len, buf, buf_size);
		memcpy(sd.m_uBuf.get() + header_len + buf_size, "\r\n", 2);
		sd.m_len = total_chunk_size;
		std::lock_guard<std::mutex> lock(m_queDataMutex);
		m_queData.emplace(std::move(sd));
	} else {
		if (m_sock->BlockSend((const char *)chunk_header, header_len) < 0) {
			MS_LOG_ERROR("http sink send chunk header failed");
			this->PassiveClose();
			return buf_size;
		}

		const char *pBuf = (const char *)buf;
		int pLen = buf_size;
		int ret = 0;

		while (pLen > 0) {
			int psend = 0;
			m_sock->Send(pBuf, pLen, &psend);
			if (psend > 0) {
				pLen -= psend;
				pBuf += psend;
			}

			if (ret >= 0) {
				// do nothing, handled in psend
			} else if (ret == MS_TRY_AGAIN) {
				SData sd;
				sd.m_uBuf = std::make_unique<uint8_t[]>(pLen + 2);
				memcpy(sd.m_uBuf.get(), pBuf, pLen);
				memcpy(sd.m_uBuf.get() + pLen, "\r\n", 2);
				sd.m_len = pLen + 2;
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
				this->PassiveClose();
				return buf_size;
			}
		}

		if (m_sock->BlockSend("\r\n", 2) < 0) {
			MS_LOG_ERROR("http sink send chunk tail failed");
			this->PassiveClose();
			return buf_size;
		}
	}

	return buf_size;
}

void MsHttpSink::SinkActiveClose() {
	m_error = true;

	this->DetachSource();
	this->SinkReleaseRes();
}

// TODO: need fix
void MsHttpSink::PassiveClose() {
	m_error = true;
	// this->DetachSourceNoLock();
	// this->SinkReleaseRes();
}

void MsHttpSink::SinkReleaseRes() {
	this->clear_que();

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
}

void MsHttpSink::OnSourceClose() {
	m_error = true;
	this->SinkReleaseRes();
}

void MsHttpSink::OnStreamPacket(AVPacket *pkt) {
	if (!m_streamReady || m_error) {
		return;
	}

	int ret;
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

	/* copy packet */
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

	ret = av_write_frame(m_fmtCtx, pkt);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_strerror(ret, errbuf, sizeof(errbuf));
		MS_LOG_ERROR("Error writing frame to HTTP sink, ret:%d %s", ret, errbuf);
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

void MsHttpSink::clear_que() {
	while (m_queData.size()) {
		m_queData.pop();
	}
	while (m_queAudioPkts.size()) {
		AVPacket *pkt = m_queAudioPkts.front();
		m_queAudioPkts.pop();
		av_packet_free(&pkt);
	}
}
