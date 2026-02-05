#include "MsSourceFactory.h"
#include "MsDevMgr.h"
#include "MsFileSource.h"
#include "MsGbSource.h"
#include "MsJtSource.h"
#include "MsLog.h"
#include "MsRtspSource.h"
#include <fstream>

static int m_seqID = 1;
static std::mutex m_mutex;

std::shared_ptr<MsMediaSource> MsSourceFactory::CreateLiveSource(const std::string &streamID) {
	auto parts = SplitString(streamID, "-");
	string deviceId = streamID;
	int streamNumber = 0;
	if (parts.size() == 2) {
		streamNumber = std::stoi(parts[1]);
		deviceId = parts[0];
	}

	auto device = MsDevMgr::Instance()->FindDevice(deviceId);
	if (!device) {
		// if streamID ends with _jt, it is a JT source
		if (streamID.size() > 3 && streamID.substr(streamID.size() - 3) == "_jt") {
			return std::make_shared<MsJtSource>(streamID);
		}

		MS_LOG_WARN("device:%s not found", streamID.c_str());
		return nullptr;
	}

	switch (device->m_protocol) {
	case RTSP_DEV: {
		std::string url = device->m_url;
		if (url.empty()) {
			MS_LOG_WARN("device:%s has empty url", streamID.c_str());
			return nullptr;
		}

		return std::make_shared<MsRtspSource>(streamID, url);
	} break;

	case GB_DEV: {
		std::shared_ptr<SGbContext> ctx = std::make_shared<SGbContext>();
		ctx->gbID = device->m_deviceID;
		ctx->type = 0;
		ctx->startTime = "0";
		ctx->endTime = "0";
		ctx->streamNumber = streamNumber;

		std::lock_guard<std::mutex> lk(m_mutex);
		return std::make_shared<MsGbSource>(streamID, ctx, m_seqID++);
	} break;

	default:
		return nullptr;
	}
}

std::shared_ptr<MsMediaSource> MsSourceFactory::CreateVodSource(const std::string &streamID,
                                                                const std::string &filename) {
	std::string fn = "files/" + filename;

	// Check if file exists
	std::ifstream file(fn);
	if (!file.good()) {
		MS_LOG_WARN("file not found: %s", fn.c_str());
		return nullptr;
	}

	return std::make_shared<MsFileSource>(streamID, fn);
}

std::shared_ptr<MsMediaSource> MsSourceFactory::CreateGbvodSource(const std::string &streamID,
                                                                  const std::string &streamInfo) {
	//%s-%ld-%ld-%d", devID.c_str(), nSt, nEt, nType
	std::vector<std::string> parts = SplitString(streamInfo, "-");
	if (parts.size() != 4) {
		MS_LOG_WARN("invalid gbvod streamInfo:%s", streamInfo.c_str());
		return nullptr;
	}

	std::shared_ptr<SGbContext> ctx = std::make_shared<SGbContext>();
	ctx->gbID = parts[0];
	ctx->type = std::stoi(parts[3]);
	ctx->startTime = parts[1];
	ctx->endTime = parts[2];
	std::lock_guard<std::mutex> lk(m_mutex);
	return std::make_shared<MsGbSource>(streamID, ctx, m_seqID++);
}
