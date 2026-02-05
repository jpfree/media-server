#include "MsRtcServer.h"
#include "MsCommon.h"
#include "MsConfig.h"
#include "MsResManager.h"
#include "MsRtcSink.h"

void MsRtcServer::Run() {
	this->RegistToManager();

	rtc::InitLogger(rtc::LogLevel::Debug);

	std::thread worker(&MsReactor::Wait, shared_from_this());
	worker.detach();
}

void MsRtcServer::HandleMsg(MsMsg &msg) {
	switch (msg.m_msgID) {
	case MS_HTTP_TRANSFER_MSG: {
		shared_ptr<SHttpTransferMsg> rtcMsg;
		try {
			rtcMsg = std::any_cast<shared_ptr<SHttpTransferMsg>>(msg.m_any);
		} catch (std::bad_any_cast &e) {
			MS_LOG_WARN("rtc msg bad any cast:%s", e.what());
			return;
		}
		this->RtcProcess(rtcMsg);
	} break;

	case MS_RTC_PEER_CLOSED: {
		string sessionId = msg.m_strVal;
		std::lock_guard<std::mutex> lock(m_mtx);
		auto it = m_pcMap.find(sessionId);
		if (it != m_pcMap.end()) {
			it->second->SourceActiveClose();
			m_pcMap.erase(it);
			MS_LOG_INFO("pc:%s removed", sessionId.c_str());
		}
	} break;

	case MS_RTC_DEL_SOCK: {
		string sessionId = msg.m_strVal;
		std::lock_guard<std::mutex> lock(m_mtx);
		auto it = m_pcMap.find(sessionId);
		if (it != m_pcMap.end()) {
			it->second->_sock.reset();
			MS_LOG_INFO("pc:%s sock reset", sessionId.c_str());
		}
		auto itWhep = m_whepSinkMap.find(sessionId);
		if (itWhep != m_whepSinkMap.end()) {
			itWhep->second->_sock.reset();
			MS_LOG_INFO("whep:%s sock reset", sessionId.c_str());
		}
	} break;

	case MS_WHEP_PEER_CLOSED: {
		string sessionId = msg.m_strVal;
		std::lock_guard<std::mutex> lock(m_mtx);
		auto it = m_whepSinkMap.find(sessionId);
		if (it != m_whepSinkMap.end()) {
			it->second->SinkActiveClose();
			m_whepSinkMap.erase(it);
			MS_LOG_INFO("whep:%s removed", sessionId.c_str());
		}
	} break;

	default:
		MsReactor::HandleMsg(msg);
		break;
	}
}

void MsRtcServer::RtcProcess(shared_ptr<SHttpTransferMsg> rtcMsg) {
	// Handle OPTIONS request
	MsHttpMsg &msg = rtcMsg->httpMsg;
	shared_ptr<MsSocket> &sock = rtcMsg->sock;

	if (msg.m_method == "OPTIONS") {
		MsHttpMsg rsp;
		rsp.m_version = msg.m_version;
		rsp.m_status = "204";
		rsp.m_reason = "No Content";
		SendHttpRspEx(sock.get(), rsp);
	} else if (msg.m_uri.find("whip") != string::npos) {
		return this->WhipProcess(rtcMsg);
	}
	// if httpMsg.uri contains "whep" then whep process
	else if (msg.m_uri.find("whep") != string::npos) {
		return this->WhepProcess(rtcMsg);
	}
	// if uri start with /rtc/session then rtc process
	else if (msg.m_uri.find("/rtc/session") == 0) {
		// if uri has /url, get the playback url
		if (msg.m_uri.find("/url") != string::npos) {
			string sessionId;
			GetParam("sessionId", sessionId, msg.m_uri);
			{
				std::lock_guard<std::mutex> lock(m_mtx);
				auto it = m_pcMap.find(sessionId);
				if (it == m_pcMap.end()) {
					MS_LOG_WARN("pc:%s not found", sessionId.c_str());
					MsHttpMsg rsp;
					rsp.m_version = msg.m_version;
					rsp.m_status = "404";
					rsp.m_reason = "Not Found";
					SendHttpRspEx(sock.get(), rsp);
					return;
				}
			}
#if ENABLE_HTTPS
			string protocol = "https://";
#else
			string protocol = "http://";
#endif

			string httpIp = MsConfig::Instance()->GetConfigStr("localBindIP");
			int httpPort = MsConfig::Instance()->GetConfigInt("httpPort");

			json j;
			char bb[512];
			sprintf(bb, "%s%s:%d/live/%s.flv", protocol.c_str(), httpIp.c_str(), httpPort,
			        sessionId.c_str());
			j["httpFlvUrl"] = bb;

			sprintf(bb, "%s%s:%d/live/%s.ts", protocol.c_str(), httpIp.c_str(), httpPort,
			        sessionId.c_str());
			j["httpTsUrl"] = bb;

			sprintf(bb, "%s%s:%d/rtc/whep/%s", protocol.c_str(), httpIp.c_str(), httpPort,
			        sessionId.c_str());
			j["rtcUrl"] = bb;

			sprintf(bb, "rtsp://%s:%d/live/%s", httpIp.c_str(), httpPort, sessionId.c_str());
			j["rtspUrl"] = bb;

			json rsp;
			rsp["code"] = 0;
			rsp["msg"] = "success";
			rsp["result"] = j;
			SendHttpRsp(sock.get(), rsp.dump());
		} else {
			// return all whip session with info
			json j, sessions = json::array();
			{
				std::lock_guard<std::mutex> lock(m_mtx);
				for (auto &it : m_pcMap) {
					json session;
					session["sessionId"] = it.first;
					session["videoCodec"] = it.second->_videoCodec;
					session["audioCodec"] = it.second->_audioCodec;
					sessions.push_back(session);
				}
			}
			j["code"] = 0;
			j["msg"] = "ok";
			j["result"] = sessions;
			SendHttpRsp(sock.get(), j.dump());
		}
	} else {
		MsHttpMsg rsp;
		rsp.m_version = rtcMsg->httpMsg.m_version;
		rsp.m_status = "404";
		rsp.m_reason = "Not Found";
		SendHttpRspEx(rtcMsg->sock.get(), rsp);
	}
}

void MsRtcServer::WhipProcess(shared_ptr<SHttpTransferMsg> rtcMsg) {
	MsHttpMsg &msg = rtcMsg->httpMsg;
	shared_ptr<MsSocket> &sock = rtcMsg->sock;
	string &sdp = rtcMsg->body;

	if (msg.m_method == "POST") {
		string sessionId = GenRandStr(16);
		string version = msg.m_version;
		string httpIp = MsConfig::Instance()->GetConfigStr("localBindIP");
		int httpPort = MsConfig::Instance()->GetConfigInt("httpPort");

#if ENABLE_HTTPS
		string protocol = "https://";
#else
		string protocol = "http://";
#endif

		string location =
		    protocol + httpIp + ":" + to_string(httpPort) + msg.m_uri + "/" + sessionId;

		rtc::Configuration config;
		config.enableIceTcp = true;
		config.enableIceUdpMux = true;
		config.portRangeBegin = MsConfig::Instance()->GetConfigInt("rtcPort");
		config.portRangeEnd = config.portRangeBegin;

		auto pc = make_shared<rtc::PeerConnection>(config);

		int expectedTracks = 0;
		rtc::Description offer(sdp, "offer");
		for (int i = 0; i < offer.mediaCount(); ++i) {
			auto var = offer.media(i);
			auto media = std::get_if<rtc::Description::Media *>(&var);
			if (media == nullptr) {
				continue;
			}
			string type = (*media)->type();
			if (type == "audio" || type == "video") {
				expectedTracks++;
			}
		}

		shared_ptr<MsRtcSource> peerConn = make_shared<MsRtcSource>(sessionId);
		peerConn->_pc = pc;
		peerConn->_sock = sock;
		peerConn->_sessionId = sessionId;
		peerConn->_expectedTracks = expectedTracks;

		{
			std::lock_guard<std::mutex> lock(m_mtx);
			m_pcMap[sessionId] = peerConn;
		}

		pc->onStateChange([this, sessionId](rtc::PeerConnection::State state) {
			// just log state change, and log state string
			string strState;
			switch (state) {
			case rtc::PeerConnection::State::New:
				strState = "New";
				break;
			case rtc::PeerConnection::State::Connecting:
				strState = "Connecting";
				break;
			case rtc::PeerConnection::State::Connected:
				strState = "Connected";
				break;
			case rtc::PeerConnection::State::Disconnected:
				strState = "Disconnected";
				break;
			case rtc::PeerConnection::State::Failed:
				strState = "Failed";
				break;
			case rtc::PeerConnection::State::Closed: {
				strState = "Closed";
				// callback must not use lock
				MsMsg msg;
				msg.m_msgID = MS_RTC_PEER_CLOSED;
				msg.m_strVal = sessionId;
				this->EnqueMsg(msg);
			} break;
			}
			MS_LOG_INFO("pc:%s state:%s", sessionId.c_str(), strState.c_str());
		});

		pc->onTrack([this, sessionId](shared_ptr<rtc::Track> track) {
			weak_ptr<rtc::Track> weak_track = track;
			track->onOpen([weak_track, sessionId]() {
				auto track = weak_track.lock();
				if (track)
					MS_LOG_INFO("pc:%s track open:%s", sessionId.c_str(), track->mid().c_str());
			});

			rtc::Description::Media media = track->description();
			string mediaType = media.type();
			MS_LOG_INFO("pc:%s track %s type:%s", sessionId.c_str(), track->mid().c_str(),
			            mediaType.c_str());

			auto payloadTypes = media.payloadTypes();
			// use 1 bool is enough since we only accept one video and one audio track
			bool mediaFound = false;

			for (auto &pt : payloadTypes) {
				rtc::Description::Media::RtpMap *rtpMap = nullptr;
				try {
					rtpMap = media.rtpMap(pt);
				} catch (...) {
					continue;
				}

				if (mediaFound) {
					if (rtpMap->format != "rtx" && rtpMap->format != "red" &&
					    rtpMap->format != "ulpfec") {
						// only accept one media track, remove other rtpmap except rtx
						media.removeRtpMap(pt);
						MS_LOG_INFO("pc:%s %s track remove extra format:%s payload:%d",
						            sessionId.c_str(), mediaType.c_str(), rtpMap->format.c_str(),
						            pt);
					} else {
						// for rtx, red, ulpfec just keep it
					}

					continue;
				}

				if (IsSupportedCodec(rtpMap->format)) {
					mediaFound = true;

					MS_LOG_INFO("pc:%s %s track format:%s payload:%d", sessionId.c_str(),
					            mediaType.c_str(), rtpMap->format.c_str(), pt);

					std::lock_guard<std::mutex> lock(m_mtx);
					auto it = m_pcMap.find(sessionId);
					if (it != m_pcMap.end()) {
						auto &peerConn = it->second;
						if (mediaType == "video" && !peerConn->_videoTrack) {
							peerConn->_videoTrack = track;
							peerConn->_videoPt = pt;
							peerConn->_videoCodec = rtpMap->format;
							peerConn->_videoFmts = rtpMap->fmtps;
						} else if (mediaType == "audio" && !peerConn->_audioTrack) {
							peerConn->_audioTrack = track;
							peerConn->_audioPt = pt;
							peerConn->_audioCodec = rtpMap->format;
							peerConn->_audioFmts = rtpMap->fmtps;
						}
					}

				} else {
					media.removeRtpMap(pt);
					MS_LOG_INFO("pc:%s %s track remove unsupported format:%s payload:%d",
					            sessionId.c_str(), mediaType.c_str(), rtpMap->format.c_str(), pt);
				}

				std::shared_ptr<rtc::RtcpReceivingSession> session =
				    std::make_shared<rtc::RtcpReceivingSession>();
				track->setMediaHandler(session);

				shared_ptr<MsRtcSource> peerConn;
				{
					std::lock_guard<std::mutex> lock(m_mtx);
					auto it = m_pcMap.find(sessionId);
					if (it != m_pcMap.end()) {
						peerConn = it->second;
					}
				}

				track->onMessage(
				    [peerConn](rtc::binary message) {
					    if (peerConn) {
						    auto rtp = reinterpret_cast<rtc::RtpHeader *>(message.data());
						    if (rtp->payloadType() == peerConn->_videoPt ||
						        rtp->payloadType() == peerConn->_audioPt) {
							    peerConn->WriteBuffer(message.data(), message.size());
						    }
					    }
				    },
				    nullptr);
			}

			track->setDescription(std::move(media));

			{
				std::lock_guard<std::mutex> lock(m_mtx);
				auto it = m_pcMap.find(sessionId);
				if (it != m_pcMap.end()) {
					auto &peerConn = it->second;
					peerConn->_receivedTracks++;
					if (peerConn->_receivedTracks == peerConn->_expectedTracks) {
						MS_LOG_INFO("pc:%s all %d tracks received", sessionId.c_str(),
						            peerConn->_expectedTracks);
						peerConn->m_rtpThread =
						    std::make_unique<std::thread>(&MsRtcSource::StartRtpDemux, peerConn);
					}
				}
			}
		});

		pc->onLocalDescription([](rtc::Description description) {});

		std::weak_ptr<rtc::PeerConnection> weak_pc = pc;
		std::weak_ptr<MsSocket> weak_sock = sock;

		pc->onGatheringStateChange([this, weak_pc, weak_sock, sessionId, version,
		                            location](rtc::PeerConnection::GatheringState state) {
			if (state == rtc::PeerConnection::GatheringState::Complete) {
				auto pc = weak_pc.lock();
				if (!pc)
					return;

				auto sock = weak_sock.lock();
				if (!sock)
					return;

				auto description = pc->localDescription();
				string sdp = description->generateSdp();

				MsHttpMsg rsp;
				rsp.m_version = version;
				rsp.m_status = "201";
				rsp.m_reason = "Created";
				rsp.m_location.SetValue(location);
				rsp.m_contentType.SetValue("application/sdp");
				rsp.m_exposeHeader.SetValue("Location");

				rsp.SetBody(sdp.c_str(), sdp.size());
				SendHttpRspEx(sock.get(), rsp);

				MsMsg msg;
				msg.m_msgID = MS_RTC_DEL_SOCK;
				msg.m_strVal = sessionId;
				this->EnqueMsg(msg);
			}
		});

		try {
			pc->setRemoteDescription(std::move(offer));
		} catch (const std::exception &e) {
			printf("pc:%s gather candidates exception:%s\n", sessionId.c_str(), e.what());
		}

	} else if (msg.m_method == "DELETE") {
		size_t pos = msg.m_uri.find_last_of('/');
		if (pos != string::npos && pos < msg.m_uri.size() - 1) {
			string sessionId = msg.m_uri.substr(pos + 1);
			std::lock_guard<std::mutex> lock(m_mtx);
			auto it = m_pcMap.find(sessionId);
			if (it != m_pcMap.end()) {
				it->second->SourceActiveClose();
				m_pcMap.erase(it);
			}
		}

		MsHttpMsg rsp;
		rsp.m_version = msg.m_version;
		rsp.m_status = "200";
		rsp.m_reason = "OK";
		SendHttpRspEx(sock.get(), rsp);
	} else if (msg.m_method == "GET") {
		// return 204 for GET method
		MsHttpMsg rsp;
		rsp.m_version = msg.m_version;
		rsp.m_status = "204";
		rsp.m_reason = "No Content";
		SendHttpRspEx(sock.get(), rsp);
	} else {
		MsHttpMsg rsp;
		rsp.m_version = msg.m_version;
		rsp.m_status = "405";
		rsp.m_reason = "Method Not Allowed";

		string body = "405 Method Not Allowed";
		rsp.m_contentLength.SetIntVal(body.size());
		rsp.m_body = body.c_str();
		rsp.m_bodyLen = body.size();
		rsp.m_contentType.SetValue("text/plain; charset=UTF-8");

		SendHttpRspEx(sock.get(), rsp);
	}
}

void MsRtcServer::WhepProcess(shared_ptr<SHttpTransferMsg> rtcMsg) {
	MsHttpMsg &msg = rtcMsg->httpMsg;
	shared_ptr<MsSocket> &sock = rtcMsg->sock;
	string &sdp = rtcMsg->body;
	if (msg.m_method == "POST") {
		// Parse stream ID from URI: /whep/{streamId}
		string streamId;
		size_t pos = msg.m_uri.find("whep/");
		if (pos != string::npos) {
			string path = msg.m_uri.substr(pos + 5); // after "whep/"
			// Remove query parameters if any
			size_t qpos = path.find('?');
			if (qpos != string::npos) {
				path = path.substr(0, qpos);
			}
			streamId = path;
		}

		if (streamId.empty()) {
			MsHttpMsg rsp;
			rsp.m_version = msg.m_version;
			rsp.m_status = "400";
			rsp.m_reason = "Bad Request";
			SendHttpRspEx(sock.get(), rsp);
			return;
		}

		string sessionId = GenRandStr(16);
		string httpIp = MsConfig::Instance()->GetConfigStr("localBindIP");
		int httpPort = MsConfig::Instance()->GetConfigInt("httpPort");

#if ENABLE_HTTPS
		string protocol = "https://";
#else
		string protocol = "http://";
#endif

		string location = protocol + httpIp + ":" + to_string(httpPort) + "/rtc/whep/" + streamId +
		                  "/" + sessionId;

		// Create the RTC sink
		static int whepSinkId = 0;
		shared_ptr<MsRtcSink> rtcSink =
		    make_shared<MsRtcSink>("whep", streamId, ++whepSinkId, sessionId);
		rtcSink->_sock = sock;
		rtcSink->_sessionId = sessionId;

		// Setup WebRTC parameters - the actual tracks will be created in OnStreamInfo
		// after we know the stream codec info
		auto weakThis = weak_from_this();
		rtcSink->SetupWebRTC(sdp, msg.m_version, location, [weakThis](const string &sid) {
			auto self = weakThis.lock();
			if (self) {
				MsMsg closeMsg;
				closeMsg.m_msgID = MS_WHEP_PEER_CLOSED;
				closeMsg.m_strVal = sid;
				self->EnqueMsg(closeMsg);
			}
		});

		{
			std::lock_guard<std::mutex> lock(m_mtx);
			m_whepSinkMap[sessionId] = rtcSink;
		}

		// Check if the source exists
		auto source =
		    MsResManager::GetInstance().GetOrCreateMediaSource("live", streamId, "", rtcSink);

		if (!source) {
			MS_LOG_WARN("whep stream %s not found", streamId.c_str());
			MsHttpMsg rsp;
			rsp.m_version = msg.m_version;
			rsp.m_status = "404";
			rsp.m_reason = "Not Found";
			SendHttpRspEx(sock.get(), rsp);
			rtcSink->SinkActiveClose();
			return;
		}

		MS_LOG_INFO("whep sink %s added to source %s", sessionId.c_str(), streamId.c_str());

	} else if (msg.m_method == "DELETE") {
		size_t pos = msg.m_uri.find_last_of('/');
		if (pos != string::npos && pos < msg.m_uri.size() - 1) {
			string sessionId = msg.m_uri.substr(pos + 1);
			std::lock_guard<std::mutex> lock(m_mtx);
			auto it = m_whepSinkMap.find(sessionId);
			if (it != m_whepSinkMap.end()) {
				it->second->DetachSource();
				m_whepSinkMap.erase(it);
			}
		}

		MsHttpMsg rsp;
		rsp.m_version = msg.m_version;
		rsp.m_status = "200";
		rsp.m_reason = "OK";
		SendHttpRspEx(sock.get(), rsp);
	} else if (msg.m_method == "GET") {
		// return 204 for GET method
		MsHttpMsg rsp;
		rsp.m_version = msg.m_version;
		rsp.m_status = "204";
		rsp.m_reason = "No Content";
		SendHttpRspEx(sock.get(), rsp);
	} else {
		MsHttpMsg rsp;
		rsp.m_version = msg.m_version;
		rsp.m_status = "405";
		rsp.m_reason = "Method Not Allowed";

		string body = "405 Method Not Allowed";
		rsp.m_contentLength.SetIntVal(body.size());
		rsp.m_body = body.c_str();
		rsp.m_bodyLen = body.size();
		rsp.m_contentType.SetValue("text/plain; charset=UTF-8");

		SendHttpRspEx(sock.get(), rsp);
	}
}

bool MsRtcServer::IsSupportedCodec(const string &codec) {
	return (codec == "OPUS" || codec == "opus" || codec == "H264" || codec == "H265");
}
