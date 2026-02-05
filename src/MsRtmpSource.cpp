#include "MsRtmpSource.h"
#include "MsResManager.h"

void MsRtmpSource::Work() {
	if (m_flvThread)
		return;

	// write flv header to ring buffer
	uint8_t flv_header[13] = {'F',  'L',  'V',  0x01, 0x05, 0x00, 0x00,
	                          0x00, 0x09, 0x00, 0x00, 0x00, 0x00};

	m_ringBuffer->write((char *)flv_header, sizeof(flv_header));

	m_flvThread = std::make_unique<std::thread>(&MsRtmpSource::FlvParseThread, this);
}

void MsRtmpSource::AddSink(std::shared_ptr<MsMediaSink> sink) {
	if (!sink || m_isClosing.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(m_sinkMutex);
	m_sinks.push_back(sink);
	if (m_video || m_audio) {
		sink->OnStreamInfo(m_video, m_videoIdx, m_audio, m_audioIdx);
	}
}

void MsRtmpSource::NotifyStreamPacket(AVPacket *pkt) {
	std::lock_guard<std::mutex> lock(m_sinkMutex);
	for (auto &sink : m_sinks) {
		if (sink) {
			sink->OnStreamPacket(pkt);
		}
	}
}

void MsRtmpSource::SourceActiveClose() {
	m_isClosing.store(true);
	m_condVar.notify_all();

	if (m_flvThread) {
		if (m_flvThread->joinable())
			m_flvThread->join();
		m_flvThread.reset();
	}

	MsMediaSource::SourceActiveClose();
}

void MsRtmpSource::FlvParseThread() {
	AVFormatContext *fmt_ctx = NULL;
	AVIOContext *avio_ctx = NULL;
	const AVInputFormat *fmt = NULL;
	uint8_t *avio_ctx_buffer = NULL;
	AVPacket *pkt = NULL;
	AVDictionary *options = NULL;
	size_t avio_ctx_buffer_size = 4096;
	int ret = 0;

	MsResManager::GetInstance().AddMediaSource(m_streamID, this->GetSharedPtr());

	if (!(fmt_ctx = avformat_alloc_context())) {
		MS_LOG_ERROR("Could not allocate format context");
		ret = AVERROR(ENOMEM);
		goto end;
	}

	avio_ctx_buffer = (uint8_t *)av_malloc(avio_ctx_buffer_size);
	if (!avio_ctx_buffer) {
		MS_LOG_ERROR("Could not allocate avio context buffer");
		ret = AVERROR(ENOMEM);
		goto end;
	}

	avio_ctx = avio_alloc_context(
	    avio_ctx_buffer, avio_ctx_buffer_size, 0, this,
	    [](void *opaque, uint8_t *buf, int buf_size) -> int {
		    MsRtmpSource *source = static_cast<MsRtmpSource *>(opaque);
		    return source->ReadBuffer(buf, buf_size);
	    },
	    NULL, NULL);

	if (!avio_ctx) {
		MS_LOG_ERROR("Could not allocate avio context");
		av_freep(&avio_ctx_buffer);
		ret = AVERROR(ENOMEM);
		goto end;
	}
	fmt_ctx->pb = avio_ctx;

	fmt = av_find_input_format("flv");
	av_dict_set(&options, "analyzeduration", "200000", 0);
	ret = avformat_open_input(&fmt_ctx, NULL, fmt, &options);
	if (options)
		av_dict_free(&options);
	if (ret < 0) {
		MS_LOG_ERROR("Could not open input");
		goto end;
	}

	ret = avformat_find_stream_info(fmt_ctx, NULL);
	if (ret < 0) {
		MS_LOG_ERROR("Could not find stream information");
		goto end;
	}

	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (ret < 0) {
		MS_LOG_ERROR("Could not find video stream in input:%s", m_streamID.c_str());
		goto end;
	}

	m_videoIdx = ret;
	m_video = fmt_ctx->streams[m_videoIdx];
	if (!MsMediaSource::IsSupportedCodec(m_video->codecpar->codec_id)) {
		MS_LOG_ERROR("not support codec:%d url:%s", m_video->codecpar->codec_id,
		             m_streamID.c_str());
		goto end;
	}

	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (ret >= 0) {
		AVCodecParameters *codecpar = fmt_ctx->streams[ret]->codecpar;
		MS_LOG_DEBUG("audio codec_id:%d channels:%d sample_rate:%d format:%d extradata_size:%d",
		             codecpar->codec_id, codecpar->ch_layout.nb_channels, codecpar->sample_rate,
		             codecpar->format, codecpar->extradata_size);

		if (fmt_ctx->streams[ret]->codecpar->codec_id == AV_CODEC_ID_AAC ||
		    fmt_ctx->streams[ret]->codecpar->codec_id == AV_CODEC_ID_OPUS) {
			m_audioIdx = ret;
			m_audio = fmt_ctx->streams[m_audioIdx];
		}
	}

	this->NotifyStreamInfo();

	pkt = av_packet_alloc();

	/* read frames from the file */
	while (av_read_frame(fmt_ctx, pkt) >= 0 && !m_isClosing.load()) {
		if (pkt->stream_index == m_videoIdx || pkt->stream_index == m_audioIdx) {
			// if (pkt->stream_index == m_videoIdx) {
			// 	MS_LOG_DEBUG("gb source video pkt pts:%ld dts:%ld key:%d", pkt->pts, pkt->dts,
			// 	             pkt->flags & AV_PKT_FLAG_KEY);
			// } else if (pkt->stream_index == m_audioIdx) {
			// 	MS_LOG_DEBUG("gb source audio pkt pts:%ld dts:%ld size:%d", pkt->pts, pkt->dts,
			// 	             pkt->size);
			// }
			this->NotifyStreamPacket(pkt);
		}
		av_packet_unref(pkt);
	}

end:
	if (fmt_ctx) {
		if (fmt_ctx->pb) {
			avio_flush(fmt_ctx->pb);
			av_freep(&fmt_ctx->pb->buffer);
			avio_context_free(&fmt_ctx->pb);
		}
		avformat_free_context(fmt_ctx);
		fmt_ctx = nullptr;
	}
	av_packet_free(&pkt);
	if (!m_isClosing.load())
		this->SourceActiveClose();
}

int MsRtmpSource::ReadBuffer(uint8_t *buf, int buf_size) {
	std::unique_lock<std::mutex> lock(m_mutex);
	m_condVar.wait(lock, [this]() { return m_ringBuffer->size() > 0 || m_isClosing.load(); });

	if (m_isClosing.load()) {
		return AVERROR_EOF;
	}

	if (m_ringBuffer->size() <= 0) {
		return AVERROR(EAGAIN);
	}

	return m_ringBuffer->read((char *)buf, buf_size);
}

int MsRtmpSource::WirteFlvData(uint8_t msgId, uint32_t msgLength, uint32_t timestamp,
                               uint8_t *data) {
	if (m_isClosing.load()) {
		return -1;
	}

	uint8_t flv_tag_header[11];
	flv_tag_header[0] = msgId;
	flv_tag_header[1] = (msgLength >> 16) & 0xFF;
	flv_tag_header[2] = (msgLength >> 8) & 0xFF;
	flv_tag_header[3] = msgLength & 0xFF;
	flv_tag_header[4] = (timestamp >> 16) & 0xFF;
	flv_tag_header[5] = (timestamp >> 8) & 0xFF;
	flv_tag_header[6] = timestamp & 0xFF;
	flv_tag_header[7] = (timestamp >> 24) & 0xFF; // TimestampExtended
	flv_tag_header[8] = 0x00;
	flv_tag_header[9] = 0x00;
	flv_tag_header[10] = 0x00; // StreamID always

	uint8_t prev_tag_size[4];
	uint32_t total_tag_size = msgLength + 11;
	prev_tag_size[0] = (total_tag_size >> 24) & 0xFF;
	prev_tag_size[1] = (total_tag_size >> 16) & 0xFF;
	prev_tag_size[2] = (total_tag_size >> 8) & 0xFF;
	prev_tag_size[3] = total_tag_size & 0xFF;

	std::unique_lock<std::mutex> lock(m_mutex);
	m_ringBuffer->write((char *)flv_tag_header, sizeof(flv_tag_header));
	m_ringBuffer->write((char *)data, msgLength);
	m_ringBuffer->write((char *)prev_tag_size, sizeof(prev_tag_size));
	lock.unlock();
	m_condVar.notify_one();

	return 0;
}
