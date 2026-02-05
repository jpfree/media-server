#include "MsMediaSource.h"
#include "MsResManager.h"

void MsMediaSource::AddSink(std::shared_ptr<MsMediaSink> sink) {
	if (!sink || m_isClosing.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(m_sinkMutex);
	if (m_sinks.empty()) {
		MsResManager::GetInstance().AddMediaSource(m_streamID, this->GetSharedPtr());
	}

	m_sinks.push_back(sink);
	if (m_video || m_audio) {
		sink->OnStreamInfo(m_video, m_videoIdx, m_audio, m_audioIdx);
	}
}

void MsMediaSource::RemoveSink(const std::string &type, int sinkID) {
	std::unique_lock<std::mutex> lock(m_sinkMutex);
	this->RemoveSinkNoLock(type, sinkID);
}

void MsMediaSource::RemoveSinkNoLock(const std::string &type, int sinkID) {
	for (auto it = m_sinks.begin(); it != m_sinks.end(); ++it) {
		if ((*it)->m_type == type && (*it)->m_sinkID == sinkID) {
			m_sinks.erase(it);
			break;
		}
	}

	if (m_sinks.empty()) {
		this->OnSinksEmpty();
	}
}

void MsMediaSource::NotifyStreamInfo() {
	this->UpdateVideoInfo();
	std::lock_guard<std::mutex> lock(m_sinkMutex);
	for (auto &sink : m_sinks) {
		if (sink) {
			sink->OnStreamInfo(m_video, m_videoIdx, m_audio, m_audioIdx);
		}
	}
}

void MsMediaSource::NotifySourceClose() {
	std::lock_guard<std::mutex> lock(m_sinkMutex);
	for (auto &sink : m_sinks) {
		if (sink) {
			sink->OnSourceClose();
		}
	}
	m_sinks.clear();
}

string MsMediaSource::GetVideoCodec() {
	if (m_video && m_video->codecpar) {
		return avcodec_get_name(m_video->codecpar->codec_id);
	}
	return string();
}

string MsMediaSource::GetAudioCodec() {
	if (m_audio && m_audio->codecpar) {
		return avcodec_get_name(m_audio->codecpar->codec_id);
	}
	return string();
}

void MsMediaSource::NotifyStreamPacket(AVPacket *pkt) {
	std::lock_guard<std::mutex> lock(m_sinkMutex);
	for (auto &sink : m_sinks) {
		if (sink) {
			sink->OnStreamPacket(pkt);
		}
	}
	if (m_sinks.empty()) {
		this->OnSinksEmpty();
	}
}

void MsMediaSource::SourceActiveClose() {
	m_isClosing.store(true);
	MsResManager::GetInstance().RemoveMediaSource(m_streamID);
	this->NotifySourceClose();
}

void MsMediaSource::OnSinksEmpty() { m_isClosing.store(true); }

bool MsMediaSource::IsSupportedCodec(AVCodecID codec_id) {
	return codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC ||
	       codec_id == AV_CODEC_ID_AV1 || codec_id == AV_CODEC_ID_AAC ||
	       codec_id == AV_CODEC_ID_VP8 || codec_id == AV_CODEC_ID_VP9 ||
	       codec_id == AV_CODEC_ID_OPUS;
}
