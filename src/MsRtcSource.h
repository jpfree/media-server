#ifndef MS_RTC_SOURCE_H
#define MS_RTC_SOURCE_H

#include "MsMediaSource.h"
#include "MsMsgDef.h"
#include "rtc/rtc.hpp"

struct MsRtcSource : public MsMediaSource, public enable_shared_from_this<MsRtcSource> {
	MsRtcSource(const string &sessionId) : MsMediaSource(sessionId), _sessionId(sessionId) {}
	~MsRtcSource();

	void Work() override {}
	void AddSink(std::shared_ptr<MsMediaSink> sink) override;
	void NotifyStreamPacket(AVPacket *pkt) override;
	void SourceActiveClose() override;
	void OnSinksEmpty() override {}
	shared_ptr<MsMediaSource> GetSharedPtr() override { return shared_from_this(); }

	string GenVideoSdp();
	string GenAudioSdp();
	void StartRtpDemux();
	void WriteBuffer(const void *buf, int size, bool isVideo);
	int ReadBuffer(uint8_t *buf, int buf_size, bool isVideo);

	shared_ptr<rtc::PeerConnection> _pc;
	shared_ptr<rtc::Track> _videoTrack;
	shared_ptr<rtc::Track> _audioTrack;
	shared_ptr<MsSocket> _sock;
	int _videoPt = 0;
	int _audioPt = 0;
	int _expectedTracks = 0;
	int _receivedTracks = 0;
	int _readSdpPos = 0;
	string _sessionId;
	string _videoCodec;
	string _audioCodec;
	string _sdp;
	vector<string> _videoFmts;
	vector<string> _audioFmts;

	std::queue<SData> m_videoRtpQue;
	std::queue<SData> m_videoPool;
	std::mutex m_videoQueMtx;
	std::condition_variable m_videoCondVar;

	std::queue<SData> m_audioRtpQue;
	std::queue<SData> m_audioPool;
	std::mutex m_audioQueMtx;
	std::condition_variable m_audioCondVar;

	std::unique_ptr<std::thread> m_rtpThread;
};

#endif // MS_RTC_SOURCE_H
