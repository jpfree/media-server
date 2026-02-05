#include "MsRtspSource.h"
#include "MsDevMgr.h"
#include "MsLog.h"
#include <thread>

void MsRtspSource::Work() {
	std::thread worker([this]() { this->OnRun(); });
	worker.detach();
}

void MsRtspSource::UpdateVideoInfo() {
	if (m_video == nullptr) {
		MS_LOG_WARN("video stream is null, cannot update device info");
		return;
	}

	ModDev m;
	m.m_codec = avcodec_get_name(m_video->codecpar->codec_id);
	m.m_resolution =
	    to_string(m_video->codecpar->width) + "x" + to_string(m_video->codecpar->height);
	MsDevMgr::Instance()->ModifyDevice(m_streamID, m);
}

void MsRtspSource::OnRun() {
	int ret;
	AVFormatContext *fmt_ctx = NULL;
	AVPacket *pkt = NULL;
	AVDictionary *options = NULL;
	bool fisrtVideoPkt = true;

	// Add rtsp_transport=tcp option if URL is RTSP
	if (m_url.find("rtsp://") == 0 || m_url.find("RTSP://") == 0) {
		av_dict_set(&options, "rtsp_transport", "tcp", 0);
	}

	av_dict_set(&options, "analyzeduration", "200000", 0);
	ret = avformat_open_input(&fmt_ctx, m_url.c_str(), NULL, &options);
	av_dict_free(&options);

	if (ret < 0) {
		MS_LOG_ERROR("Could not open input url:%s, err:%d", m_url.c_str(), ret);
		this->SourceActiveClose();
		return;
	}

	ret = avformat_find_stream_info(fmt_ctx, NULL);
	if (ret < 0) {
		MS_LOG_ERROR("Could not find stream info url:%s, err:%d", m_url.c_str(), ret);
		avformat_close_input(&fmt_ctx);
		this->SourceActiveClose();
		return;
	}

	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (ret < 0) {
		MS_LOG_ERROR("Could not find video stream in url:%s, err:%d", m_url.c_str(), ret);
		avformat_close_input(&fmt_ctx);
		this->SourceActiveClose();
		return;
	}

	m_videoIdx = ret;
	m_video = fmt_ctx->streams[m_videoIdx];
	if (!MsMediaSource::IsSupportedCodec(m_video->codecpar->codec_id)) {
		MS_LOG_ERROR("not support codec:%d url:%s", m_video->codecpar->codec_id, m_url.c_str());
		avformat_close_input(&fmt_ctx);
		this->SourceActiveClose();
		return;
	}

	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (ret >= 0) {
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
			// log video pts dts
			// if (pkt->stream_index == m_videoIdx) {
			// 	MS_LOG_DEBUG(
			// 	    "video pkt pts:%ld dts:%ld key:%d",
			// 	    pkt->pts * 1000L * m_video->time_base.num / m_video->time_base.den,
			// 	    pkt->dts * 1000L * m_video->time_base.num / m_video->time_base.den,
			// 	    pkt->flags & AV_PKT_FLAG_KEY);
			// } else if (pkt->stream_index == m_audioIdx) {
			// 	MS_LOG_DEBUG(
			// 	    "audio pkt pts:%ld dts:%ld size:%d",
			// 	    pkt->pts * 1000L * m_audio->time_base.num / m_audio->time_base.den,
			// 	    pkt->dts * 1000L * m_audio->time_base.num / m_audio->time_base.den,
			// 	    pkt->size);
			// }

			if (pkt->stream_index == m_videoIdx) {
				if (fisrtVideoPkt) {
					fisrtVideoPkt = false;
					// TODO: quick fix for some RTSP stream with first pkt pts=dts=AV_NOPTS_VALUE
					//       need better solution
					if (pkt->pts == AV_NOPTS_VALUE) {
						MS_LOG_WARN("first video pkt pts is AV_NOPTS_VALUE, set to 0");
						pkt->pts = 0;
						if (pkt->dts == AV_NOPTS_VALUE) {
							pkt->dts = 0;
						}
					}
				}
			}
			this->NotifyStreamPacket(pkt);
		}
		av_packet_unref(pkt);
	}

	avformat_close_input(&fmt_ctx);
	av_packet_free(&pkt);
	this->SourceActiveClose();
}
