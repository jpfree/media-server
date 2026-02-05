#ifndef MS_MEDIA_SOURCE_H
#define MS_MEDIA_SOURCE_H

#include "MsLog.h"
#include "MsMediaSink.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

class MsMediaSource {
public:
	MsMediaSource(const std::string &streamID) : m_streamID(streamID) {}

	virtual ~MsMediaSource() { MS_LOG_INFO("media source %s destroyed", m_streamID.c_str()); }

	virtual void AddSink(std::shared_ptr<MsMediaSink> sink);
	void RemoveSink(const std::string &type, int sinkID);
	void RemoveSinkNoLock(const std::string &type, int sinkID);
	void NotifyStreamInfo();
	void NotifySourceClose();
	string GetVideoCodec();
	string GetAudioCodec();
	string GetStreamID() { return m_streamID; }
	virtual void UpdateVideoInfo() {}
	virtual void NotifyStreamPacket(AVPacket *pkt);
	virtual void SourceActiveClose();
	virtual void OnSinksEmpty();
	virtual void Work() = 0;
	virtual shared_ptr<MsMediaSource> GetSharedPtr() = 0;
	static bool IsSupportedCodec(AVCodecID codec_id);

protected:
	std::atomic_bool m_isClosing{false};
	AVStream *m_video = nullptr;
	int m_videoIdx = -1;
	AVStream *m_audio = nullptr;
	int m_audioIdx = -1;
	std::string m_streamID;
	std::mutex m_sinkMutex;
	std::vector<std::shared_ptr<MsMediaSink>> m_sinks;
};

#endif // MS_MEDIA_SOURCE_H
