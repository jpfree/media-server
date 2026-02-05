#ifndef MS_RTC_SERVER_H
#define MS_RTC_SERVER_H

#include "MsMsgDef.h"
#include "MsReactor.h"
#include "MsRtcSink.h"
#include "MsRtcSource.h"
#include "rtc/rtc.hpp"
#include <map>
#include <mutex>

class MsRtcServer : public MsReactor {
public:
	using MsReactor::MsReactor;

	void Run() override;
	void HandleMsg(MsMsg &msg) override;

private:
	void RtcProcess(shared_ptr<SHttpTransferMsg> rtcMsg);
	void WhipProcess(shared_ptr<SHttpTransferMsg> rtcMsg);
	void WhepProcess(shared_ptr<SHttpTransferMsg> rtcMsg);
	bool IsSupportedCodec(const string &codec);

	std::mutex m_mtx;
	std::map<string, shared_ptr<MsRtcSource>> m_pcMap;     // WHIP sessions (ingest)
	std::map<string, shared_ptr<MsRtcSink>> m_whepSinkMap; // WHEP sessions (egress)
};

#endif // MS_RTC_SERVER_H
