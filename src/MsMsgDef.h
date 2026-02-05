#ifndef MS_MSG_DEF_H
#define MS_MSG_DEF_H
#include "MsHttpMsg.h"
#include <future>
#include <memory>
#include <stdint.h>
#include <string>

using namespace std;

enum MS_MSG_ID {
	MS_REG_TIME_OUT = 0x100,
	MS_INIT_CATALOG,
	MS_HTTP_INIT_CATALOG,
	MS_CATALOG_TIME_OUT,
	MS_INIT_RECORD,
	MS_RECORD_TIME_OUT,
	MS_INIT_INVITE,
	MS_INVITE_CALL_RSP,
	MS_INVITE_TIME_OUT,
	MS_MEDIA_NODE_TIMER,
	MS_PTZ_CONTROL,
	MS_ONVIF_PROBE,
	MS_ONVIF_PROBE_TIMEOUT,
	MS_ONVIF_PROBE_FINISH,
	MS_QUERY_PRESET,
	MS_QUERY_PRESET_TIMEOUT,
	MS_GB_SERVER_HANDLER_CLOSE,
	MS_GET_REGIST_DOMAIN,
	MS_STOP_INVITE_CALL,
	MS_HTTP_STREAM_MSG,
	MS_HTTP_TRANSFER_MSG,
	MS_RTC_PEER_CLOSED,
	MS_RTC_DEL_SOCK,
	MS_WHEP_PEER_CLOSED,
	MS_SOCK_TRANSFER_MSG,
	MS_JT_SOCKET_CLOSE,
	MS_JT_REQ_TIMEOUT,
};

enum MS_SERVICE_TYPE {
	MS_HTTP_SERVER = 1,
	MS_COMMON_REACTOR,
	MS_GB_SERVER,
	MS_GB_SOURCE,
	MS_HTTP_STREAM,
	MS_RTSP_SERVER,
	MS_RTC_SERVER,
	MS_JT_SERVER,
	MS_RTMP_SERVER,
};

enum TRASNSPORT { EN_UDP = 0, EN_TCP_ACTIVE, EN_TCP_PASSIVE };

struct SGbContext {
	string gbID;
	string gbCallID;
	string rtpIP;
	int rtpPort;
	int transport;
	int type;
	int streamNumber = 0; // 0-main, 1-sub
	string startTime;
	string endTime;
};

struct SData {
	SData() = default;
	~SData() = default;
	SData(const SData &other) = delete;
	SData &operator=(const SData &other) = delete;

	SData(SData &&other) noexcept
	    : m_uBuf(std::move(other.m_uBuf)), m_len(other.m_len), m_capacity(other.m_capacity) {
		other.m_len = 0;
		other.m_capacity = 0;
	}
	SData &operator=(SData &&other) {
		if (this != &other) {
			m_uBuf = std::move(other.m_uBuf);
			m_len = other.m_len;
			m_capacity = other.m_capacity;
			other.m_len = 0;
			other.m_capacity = 0;
		}
		return *this;
	}

	unique_ptr<uint8_t[]> m_uBuf;
	int m_len;
	int m_capacity;
};

struct SMediaNode {
	int node_id;
	int idle;
	int m_lastUsed;
	int httpPort;
	string httpMediaIP;
	string nodeIp;
};

class SPtzCmd {
public:
	string m_devid;
	string m_presetID;
	int m_ptzCmd;
	int m_timeout;
};

struct SHttpTransferMsg {
	MsHttpMsg httpMsg;
	string body;
	shared_ptr<MsSocket> sock;
};

struct SSockTransferMsg {
	shared_ptr<MsSocket> sock;
	vector<uint8_t> data;
};

struct SJtStreamInfo {
	// AV attributes from 0x1003
	uint8_t m_audioCodec = 0;
	uint8_t m_audioChannels = 0;
	uint8_t m_audioSampleRate = 0;
	uint8_t m_audioBits = 0;
	uint16_t m_audioFrameLen = 0;
	uint8_t m_audioOutput = 0;
	uint8_t m_videoCodec = 0;
	uint8_t m_maxAudioChannels = 0;
	uint8_t m_maxVideoChannels = 0;
};

struct SJtStartStreamReq {
	string m_terminalPhone;
	string m_ip;
	uint16_t m_tcpPort = 0;
	uint16_t m_udpPort = 0;
	uint8_t m_channel = 1;    // default channel 1-main driver
	uint8_t m_dataType = 0;   // default video and audio
	uint8_t m_streamType = 0; // default main stream
	std::promise<int> m_promise;
};

struct SJtChannelItem {
	uint8_t m_phyChanId;
	uint8_t m_logicChanId;
	uint8_t m_chanType; // 0-av, 1-audio, 2-video
	uint8_t m_ptz;      // 0-no, 1-yes
};

struct SJtChannelInfo {
	uint8_t m_avChannelTotal;
	uint8_t m_audioChannelTotal;
	uint8_t m_videoChannelTotal;
	std::vector<SJtChannelItem> m_channels;
};

#endif // MS_MSG_DEF_H
