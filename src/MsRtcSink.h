#ifndef MS_RTC_SINK_H
#define MS_RTC_SINK_H

#include "MsMediaSink.h"
#include "MsSocket.h"
#include "rtc/rtc.hpp"
#include <functional>
#include <queue>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

class MsRtcSink : public MsMediaSink, public enable_shared_from_this<MsRtcSink> {
public:
	MsRtcSink(const std::string &type, const std::string &streamID, int sinkID,
	          const std::string &sessionId)
	    : MsMediaSink(type, streamID, sinkID), _sessionId(sessionId) {}
	~MsRtcSink() = default;

	void OnStreamInfo(AVStream *video, int videoIdx, AVStream *audio, int audioIdx) override;
	void OnSourceClose() override;
	void OnStreamPacket(AVPacket *pkt) override;
	void SinkActiveClose();

	int WriteBuffer(const uint8_t *buf, int buf_size, int8_t isVideo);

	// Setup WebRTC related info before adding to source
	void SetupWebRTC(const string &offerSdp, const string &httpVersion, const string &location,
	                 std::function<void(const string &)> onWhepPeerClosed);

	shared_ptr<rtc::PeerConnection> _pc;
	shared_ptr<rtc::Track> _videoTrack;
	shared_ptr<rtc::Track> _audioTrack;
	shared_ptr<MsSocket> _sock;

	string _sessionId;
	string _videoCodec;
	string _audioCodec;
	int _videoPt = 0;
	int _audioPt = 0;
	uint32_t _videoSsrc = 1;
	uint32_t _audioSsrc = 2;

	// For delayed SDP answer creation
	string _offerSdp;
	string _httpVersion;
	string _location;
	std::function<void(const string &)> _onWhepPeerClosed;

private:
	void SinkReleaseRes();
	int CreateTracksAndAnswer();
	int InitAacToOpusTranscoder();
	void ReleaseTranscoder();
	void ProcessPkt(AVPacket *pkt);
	void TranscodeAAC(AVPacket *pkt, std::vector<AVPacket *> &opusPkts);
	bool IsVideoMatch(const string &codec, AVCodecID codec_id);

	bool m_streamReady = false;
	bool m_error = false;
	bool m_firstVideo = true;
	bool m_firstAudio = true;
	int64_t m_firstVideoPts = 0;
	int64_t m_firstVideoDts = 0;
	int64_t m_firstAudioPts = 0;
	int64_t m_firstAudioDts = 0;
	bool m_needTranscode = false;

	std::queue<AVPacket *> m_queAudioPkts;
	std::queue<AVPacket *> m_queVideoPkts;
	std::unique_ptr<std::thread> m_muxThread;

	AVFormatContext *m_videoFmtCtx = nullptr;
	AVFormatContext *m_audioFmtCtx = nullptr;
	AVIOContext *m_videoPb = nullptr;
	AVIOContext *m_audioPb = nullptr;
	AVStream *m_outVideo = nullptr;
	AVStream *m_outAudio = nullptr;
	int m_outVideoIdx = 0;
	int m_outAudioIdx = 0;

	// AAC to Opus transcoding
	AVCodecContext *m_aacDecCtx = nullptr;
	AVCodecContext *m_opusEncCtx = nullptr;
	SwrContext *m_swrCtx = nullptr;
	AVFrame *m_decodedFrame = nullptr;
	AVFrame *m_opusFrame = nullptr;
	AVAudioFifo *m_audioFifo = nullptr;
	int64_t m_nextOpusPts = 0;
};

#endif // MS_RTC_SINK_H
