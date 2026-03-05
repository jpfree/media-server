#ifndef MS_ONVIF_HANDLER_H
#define MS_ONVIF_HANDLER_H
#include "MsDevMgr.h"
#include "MsReactor.h"
#include <future>

class MsOnvifHandler : public MsEventHandler {
public:
	MsOnvifHandler(shared_ptr<MsReactor> r, shared_ptr<MsGbDevice> dev, int sid);
	~MsOnvifHandler();

	void HandleRead(shared_ptr<MsEvent> evt);
	void HandleClose(shared_ptr<MsEvent> evt);

	static void OnvifPtzControl(string user, string passwd, string url, string profile,
	                            string presetID, int cmd, int ttout);
	static void QueryPreset(string user, string passwd, string url, string profile,
	                        shared_ptr<promise<string>> prom);

public:
	enum {
		STAGE_PROBE,
		STAGE_GET_SERVICES,
		STAGE_GET_PROFILES,
		STAGE_GET_STREAM_URI,
	};

	void proc_probe(shared_ptr<MsEvent> evt);
	void proc_get_services(shared_ptr<MsEvent> evt);
	void proc_get_profiles(shared_ptr<MsEvent> evt);
	void proc_get_stream_uri(shared_ptr<MsEvent> evt);

	void clear_evt(shared_ptr<MsEvent> evt);
	static int parse_uri(string &url, string &ip, int &port, string &uri);
	static void gen_digest(string &passwd, string &created, string &nonce, string &digest);

	shared_ptr<MsReactor> m_reactor;
	shared_ptr<MsGbDevice> m_dev;
	shared_ptr<MsEvent> m_evt;
	unique_ptr<char[]> m_bufPtr;
	int m_nrecv;
	int m_stage;
	int m_sid;
	string m_mediaurl;
	string m_ptzurl;
	string m_profile;
	string m_rtsp;
};

#endif // MS_ONVIF_HANDLER_H
