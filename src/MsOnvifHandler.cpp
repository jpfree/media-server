#include "MsOnvifHandler.h"
#include "MsDevMgr.h"
#include "MsHttpMsg.h"
#include "MsLog.h"
#include "MsMsgDef.h"
#include "MsSha1.h"
#include "MsSocket.h"
#include "tinyxml2/tinyxml2.h"
#include <string.h>
#include <thread>

// Helper: recursively find elements by local name (ignores namespace prefix)
std::vector<tinyxml2::XMLElement *> FindElementsByLocalName(tinyxml2::XMLElement *root,
                                                            const std::string &localName) {
	std::vector<tinyxml2::XMLElement *> results;
	if (!root)
		return results;

	// XMLElement::Name() returns "prefix:LocalName" or just "LocalName"
	const char *name = root->Name();
	std::string fullName(name ? name : "");
	std::string elementLocal;

	auto colonPos = fullName.find(':');
	if (colonPos != std::string::npos)
		elementLocal = fullName.substr(colonPos + 1);
	else
		elementLocal = fullName;

	if (elementLocal == localName)
		results.push_back(root);

	for (auto *child = root->FirstChildElement(); child; child = child->NextSiblingElement()) {
		auto sub = FindElementsByLocalName(child, localName);
		results.insert(results.end(), sub.begin(), sub.end());
	}
	return results;
}

// Helper: find first element by local name (ignores namespace prefix), depth-first
tinyxml2::XMLElement *FindFirstByLocalName(tinyxml2::XMLElement *root,
                                           const std::string &localName) {
	if (!root)
		return nullptr;

	const char *name = root->Name();
	std::string fullName(name ? name : "");
	std::string elementLocal;
	auto colonPos = fullName.find(':');
	elementLocal = (colonPos != std::string::npos) ? fullName.substr(colonPos + 1) : fullName;

	if (elementLocal == localName)
		return root;

	for (auto *child = root->FirstChildElement(); child; child = child->NextSiblingElement()) {
		auto *found = FindFirstByLocalName(child, localName);
		if (found)
			return found;
	}
	return nullptr;
}

struct ServiceInfo {
	std::string namespace_; // e.g. "http://www.onvif.org/ver10/media/wsdl"
	std::string xaddr;      // e.g. "http://192.168.1.100/onvif/media"
	std::string version;    // e.g. "2.60"
};

struct OnvifServices {
	ServiceInfo media;  // Media service (ver10 or ver20)
	ServiceInfo media2; // Media2 service (ver20), if present
	ServiceInfo ptz;    // PTZ service
};

// Known namespace substrings to identify services
static bool IsMediaNamespace(const std::string &ns) {
	// ver10/media/wsdl  OR  ver20/media/wsdl
	return ns.find("/media/wsdl") != std::string::npos ||
	       ns.find("ver10/media") != std::string::npos;
}

static bool IsMedia2Namespace(const std::string &ns) {
	return ns.find("ver20/media/wsdl") != std::string::npos;
}

static bool IsPTZNamespace(const std::string &ns) {
	return ns.find("/ptz/wsdl") != std::string::npos || ns.find("ver20/ptz") != std::string::npos;
}

OnvifServices ParseGetServicesResponse(const char *xmlResponse) {
	OnvifServices services;

	tinyxml2::XMLDocument doc;
	tinyxml2::XMLError err = doc.Parse(xmlResponse);
	if (err != tinyxml2::XML_SUCCESS) {
		MS_LOG_ERROR("XML parse error: %s", doc.ErrorStr());
		return services;
	}

	tinyxml2::XMLElement *root = doc.RootElement();
	if (!root) {
		MS_LOG_ERROR("Empty XML document");
		return services;
	}

	// Each <Service> block describes one ONVIF service
	auto serviceElems = FindElementsByLocalName(root, "Service");

	for (auto *serviceElem : serviceElems) {
		// <Namespace> — identifies which service this is
		auto *nsElem = FindFirstByLocalName(serviceElem, "Namespace");
		if (!nsElem || !nsElem->GetText())
			continue;
		std::string ns(nsElem->GetText());

		// <XAddr> — the endpoint URL
		auto *xaddrElem = FindFirstByLocalName(serviceElem, "XAddr");
		std::string xaddr = (xaddrElem && xaddrElem->GetText()) ? xaddrElem->GetText() : "";

		// <Version> — optional, grab Major.Minor
		std::string version;
		auto *versionElem = FindFirstByLocalName(serviceElem, "Version");
		if (versionElem) {
			auto *major = FindFirstByLocalName(versionElem, "Major");
			auto *minor = FindFirstByLocalName(versionElem, "Minor");
			if (major && major->GetText() && minor && minor->GetText())
				version = std::string(major->GetText()) + "." + minor->GetText();
		}

		ServiceInfo info{ns, xaddr, version};

		if (IsMedia2Namespace(ns))
			services.media2 = info;
		else if (IsMediaNamespace(ns))
			services.media = info;
		else if (IsPTZNamespace(ns))
			services.ptz = info;
	}

	return services;
}

std::vector<std::string> ParseXAddrs(const char *xmlResponse) {
	std::vector<std::string> xaddrsList;

	tinyxml2::XMLDocument doc;
	tinyxml2::XMLError err = doc.Parse(xmlResponse);
	if (err != tinyxml2::XML_SUCCESS) {
		MS_LOG_ERROR("Failed to parse XML: %s", doc.ErrorStr());
		return xaddrsList;
	}

	tinyxml2::XMLElement *root = doc.RootElement();
	if (!root) {
		MS_LOG_ERROR("Empty XML document");
		return xaddrsList;
	}

	// Find all <ProbeMatch> elements
	auto probeMatches = FindElementsByLocalName(root, "ProbeMatch");

	for (auto *probeMatch : probeMatches) {
		// Find <XAddrs> inside this <ProbeMatch>
		auto xaddrsElems = FindElementsByLocalName(probeMatch, "XAddrs");
		for (auto *xaddrsElem : xaddrsElems) {
			const char *text = xaddrsElem->GetText();
			if (text) {
				// XAddrs may contain multiple space-separated URLs
				std::string xaddrs(text);
				std::istringstream iss(xaddrs);
				std::string url;
				while (iss >> url) {
					xaddrsList.push_back(url);
				}
			}
		}
	}

	return xaddrsList;
}

std::string ParseFirstProfileToken(const char *xmlResponse) {
	tinyxml2::XMLDocument doc;
	tinyxml2::XMLError err = doc.Parse(xmlResponse);
	if (err != tinyxml2::XML_SUCCESS) {
		MS_LOG_ERROR("XML parse error: %s", doc.ErrorStr());
		return "";
	}

	tinyxml2::XMLElement *root = doc.RootElement();
	if (!root) {
		MS_LOG_ERROR("Empty XML document");
		return "";
	}

	// Find the first <Profiles> element — the token is an XML attribute
	tinyxml2::XMLElement *profilesElem = FindFirstByLocalName(root, "Profiles");
	if (!profilesElem) {
		MS_LOG_ERROR("No <Profiles> element found");
		return "";
	}

	// token is an XML attribute on <Profiles token="...">
	const char *token = profilesElem->Attribute("token");
	if (!token) {
		MS_LOG_ERROR("<Profiles> element has no 'token' attribute");
		return "";
	}

	return std::string(token);
}

std::string ParseStreamUri(const char *xmlResponse) {
	tinyxml2::XMLDocument doc;
	tinyxml2::XMLError err = doc.Parse(xmlResponse);
	if (err != tinyxml2::XML_SUCCESS) {
		MS_LOG_ERROR("XML parse error: %s", doc.ErrorStr());
		return "";
	}

	tinyxml2::XMLElement *root = doc.RootElement();
	if (!root) {
		MS_LOG_ERROR("Empty XML document");
		return "";
	}

	// Structure: <GetStreamUriResponse> -> <MediaUri> -> <Uri>
	tinyxml2::XMLElement *mediaUriElem = FindFirstByLocalName(root, "MediaUri");
	if (!mediaUriElem) {
		MS_LOG_ERROR("No <MediaUri> element found");
		return "";
	}

	tinyxml2::XMLElement *uriElem = FindFirstByLocalName(mediaUriElem, "Uri");
	if (!uriElem || !uriElem->GetText()) {
		MS_LOG_ERROR("No <Uri> element found inside <MediaUri>");
		return "";
	}

	return std::string(uriElem->GetText());
}

struct PresetInfo {
	std::string token; // attribute: <Preset token="...">
	std::string name;  // child element: <Name>...</Name>
};

std::vector<PresetInfo> ParsePresetTokens(const char *xmlResponse) {
	std::vector<PresetInfo> presets;

	tinyxml2::XMLDocument doc;
	tinyxml2::XMLError err = doc.Parse(xmlResponse);
	if (err != tinyxml2::XML_SUCCESS) {
		MS_LOG_ERROR("XML parse error: %s", doc.ErrorStr());
		return presets;
	}

	tinyxml2::XMLElement *root = doc.RootElement();
	if (!root) {
		MS_LOG_ERROR("Empty XML document");
		return presets;
	}

	// Each <Preset token="..."> is a sibling under <GetPresetsResponse>
	auto presetElems = FindElementsByLocalName(root, "Preset");
	if (presetElems.empty()) {
		MS_LOG_ERROR("No <Preset> elements found");
		return presets;
	}

	for (auto *presetElem : presetElems) {
		// token is an XML attribute on <Preset token="...">
		const char *token = presetElem->Attribute("token");
		if (!token)
			continue; // skip malformed entries

		// <Name> is an optional child element
		std::string name;
		auto *nameElem = presetElem->FirstChildElement();
		while (nameElem) {
			const char *elemName = nameElem->Name();
			std::string fullName(elemName ? elemName : "");
			auto colonPos = fullName.find(':');
			std::string localName =
			    (colonPos != std::string::npos) ? fullName.substr(colonPos + 1) : fullName;
			if (localName == "Name" && nameElem->GetText()) {
				name = nameElem->GetText();
				break;
			}
			nameElem = nameElem->NextSiblingElement();
		}

		presets.push_back({std::string(token), name});
	}

	return presets;
}

MsOnvifHandler::MsOnvifHandler(shared_ptr<MsReactor> r, shared_ptr<MsGbDevice> dev, int sid)
    : m_reactor(r), m_nrecv(0), m_stage(STAGE_PROBE), m_dev(dev), m_sid(sid) {
	m_bufPtr = make_unique<char[]>(DEF_BUF_SIZE);
}

MsOnvifHandler::~MsOnvifHandler() { MS_LOG_DEBUG("~MsOnvifHandler"); }

void MsOnvifHandler::HandleRead(shared_ptr<MsEvent> evt) {
	// MS_LOG_INFO("handle read");

	MsSocket *sock = evt->GetSocket();
	int ret = sock->Recv(m_bufPtr.get() + m_nrecv, DEF_BUF_SIZE - m_nrecv);
	if (ret < 1) {
		MS_LOG_INFO("read close:%d", sock->GetFd());
		this->clear_evt(evt);
		return;
	}

	m_nrecv += ret;
	m_bufPtr[m_nrecv] = '\0';

	// MS_LOG_INFO("%s", m_bufPtr.get());

	char *p = strstr(m_bufPtr.get(), "Envelope>");
	if (!p) // not full msg recved
	{
		return;
	}

	MS_LOG_INFO("%s", m_bufPtr.get());
	// find the start of XML and move it to the beginning of buffer
	char *xmlStart = strstr(m_bufPtr.get(), "<?xml");
	if (xmlStart && xmlStart != m_bufPtr.get()) {
		size_t xmlLen = m_nrecv - (xmlStart - m_bufPtr.get());
		memmove(m_bufPtr.get(), xmlStart, xmlLen);
		m_nrecv = xmlLen;
		m_bufPtr[m_nrecv] = '\0';
	}

	switch (m_stage) {
	case STAGE_PROBE:
		this->proc_probe(evt);
		break;

	case STAGE_GET_SERVICES:
		this->proc_get_services(evt);
		break;

	case STAGE_GET_PROFILES:
		this->proc_get_profiles(evt);
		break;

	case STAGE_GET_STREAM_URI:
		this->proc_get_stream_uri(evt);
		break;

	default:
		break;
	}
}

void MsOnvifHandler::HandleClose(shared_ptr<MsEvent> evt) { this->clear_evt(evt); }

void MsOnvifHandler::OnvifPtzControl(string user, string passwd, string url, string profile,
                                     string presetID, int cmd, int tout) {
	if (cmd == -1 && tout > 0) {
		SleepMs(tout);
	}

	string ip, uri;
	int port;

	if (parse_uri(url, ip, port, uri)) {
		MS_LOG_ERROR("url err:%s", url.c_str());
		return;
	}

	shared_ptr<MsSocket> tcp_sock = make_shared<MsSocket>(AF_INET, SOCK_STREAM, 0);
	string host = ip + ":" + to_string(port);

	int ret = tcp_sock->Connect(ip, port);
	if (ret < 0) {
		MS_LOG_ERROR("connect err:%s", host.c_str());
		return;
	}

	string nonce, created, digest;
	gen_digest(passwd, created, nonce, digest);

	unique_ptr<char[]> bufPtr = make_unique<char[]>(DEF_BUF_SIZE);

	if ((cmd >= 1 && cmd <= 6) || (cmd >= 11 && cmd <= 14)) {
		string x = "0";
		string y = "0";
		string z = "0";

		switch (cmd) {
		case 1:
			x = "-0.5";
			break;

		case 2:
			x = "0.5";
			break;

		case 3:
			y = "0.5";
			break;

		case 4:
			y = "-0.5";
			break;

		case 5:
			z = "0.5";
			break;

		case 6:
			z = "-0.5";
			break;

		case 11:
			x = "-0.5";
			y = "0.5";
			break;

		case 12:
			x = "0.5";
			y = "0.5";
			break;

		case 13:
			x = "-0.5";
			y = "-0.5";
			break;

		case 14:
			x = "0.5";
			y = "-0.5";
			break;

		default:
			break;
		}

		ret =
		    sprintf(bufPtr.get(),
		            "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
		            "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
		            "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
		            "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:"
		            "Username>%s</wsse:Username><wsse:Password "
		            "Type=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
		            "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
		            "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
		            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
		            "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><ContinuousMove "
		            "xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</"
		            "ProfileToken><Velocity><PanTilt x=\"%s\" y=\"%s\" "
		            "space=\"http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace\" "
		            "xmlns=\"http://www.onvif.org/ver10/schema\"/><Zoom x=\"%s\" "
		            "space=\"http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace\" "
		            "xmlns=\"http://www.onvif.org/ver10/schema\"/></Velocity></ContinuousMove></"
		            "s:Body></s:Envelope>",
		            user.c_str(), digest.c_str(), nonce.c_str(), created.c_str(), profile.c_str(),
		            x.c_str(), y.c_str(), z.c_str());
	} else if (cmd == 7) {
		ret =
		    sprintf(bufPtr.get(),
		            "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
		            "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
		            "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
		            "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:"
		            "Username>%s</wsse:Username><wsse:Password "
		            "Type=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
		            "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
		            "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
		            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
		            "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><GotoPreset "
		            "xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</"
		            "ProfileToken><PresetToken>%s</PresetToken></GotoPreset></s:Body></s:Envelope>",
		            user.c_str(), digest.c_str(), nonce.c_str(), created.c_str(), profile.c_str(),
		            presetID.c_str());
	} else if (cmd == 8) {
		ret =
		    sprintf(bufPtr.get(),
		            "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
		            "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
		            "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
		            "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:"
		            "Username>%s</wsse:Username><wsse:Password "
		            "Type=\"http://docs.oasis-open.org/wss/2004/01/"
		            "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
		            "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
		            "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
		            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
		            "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><SetPreset "
		            "xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</"
		            "ProfileToken><PresetToken>%s</PresetToken></SetPreset></s:Body></s:Envelope>",
		            user.c_str(), digest.c_str(), nonce.c_str(), created.c_str(), profile.c_str(),
		            presetID.c_str());
	} else if (cmd == 9) {
		ret = sprintf(
		    bufPtr.get(),
		    "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
		    "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
		    "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
		    "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
		    "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
		    "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:Username>%s</"
		    "wsse:Username><wsse:Password "
		    "Type=\"http://docs.oasis-open.org/wss/2004/01/"
		    "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
		    "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
		    "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
		    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
		    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><RemovePreset "
		    "xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</"
		    "ProfileToken><PresetToken>%s</PresetToken></RemovePreset></s:Body></s:Envelope>",
		    user.c_str(), digest.c_str(), nonce.c_str(), created.c_str(), profile.c_str(),
		    presetID.c_str());
	} else if (cmd == -1) {
		ret = sprintf(
		    bufPtr.get(),
		    "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
		    "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
		    "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
		    "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
		    "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
		    "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:Username>%s</"
		    "wsse:Username><wsse:Password "
		    "Type=\"http://docs.oasis-open.org/wss/2004/01/"
		    "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
		    "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
		    "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
		    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
		    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><Stop "
		    "xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</"
		    "ProfileToken><PanTilt>true</PanTilt><Zoom>true</Zoom></Stop></s:Body></s:Envelope>",
		    user.c_str(), digest.c_str(), nonce.c_str(), created.c_str(), profile.c_str());
	}

	MsHttpMsg req;
	string strReq;

	req.m_method = "POST";
	req.m_uri = uri;
	req.m_version = "HTTP/1.1";
	req.m_host.SetValue(host);
	req.m_connection.SetValue("close");
	req.m_contentType.SetValue("application/soap+xml; charset=utf-8");
	req.SetBody(bufPtr.get(), ret);
	req.Dump(strReq);

	ret = tcp_sock->Send(strReq.c_str(), strReq.size());
	if (ret < 0) {
		MS_LOG_ERROR("send err:%s", strReq.c_str());
		return;
	}

	if ((cmd >= 1 && cmd <= 6) || (cmd >= 11 && cmd <= 14)) {
		thread work(MsOnvifHandler::OnvifPtzControl, user, passwd, url, profile, presetID, -1,
		            tout);
		work.detach();
	}

	while (true) {
		ret = tcp_sock->Recv(bufPtr.get(), DEF_BUF_SIZE);
		if (ret < 1) {
			break;
		}

		bufPtr[ret] = '\0';
		char *p = strstr(bufPtr.get(), "Envelope>");
		if (p) {
			break;
		}
	}

	MS_LOG_DEBUG("ptz:%d finish", cmd);
}

void MsOnvifHandler::QueryPreset(string user, string passwd, string url, string profile,
                                 shared_ptr<promise<string>> prom) {
	json rsp;
	rsp["code"] = 0;
	rsp["msg"] = "ok";
	rsp["result"] = json::array();

	string ip, uri;
	int port;

	if (parse_uri(url, ip, port, uri)) {
		MS_LOG_ERROR("url err:%s", url.c_str());
		rsp["code"] = 1;
		rsp["msg"] = "url err";
		prom->set_value(rsp.dump());
		return;
	}

	shared_ptr<MsSocket> tcp_sock = make_shared<MsSocket>(AF_INET, SOCK_STREAM, 0);
	string host = ip + ":" + to_string(port);

	int ret = tcp_sock->Connect(ip, port);
	if (ret < 0) {
		MS_LOG_ERROR("connect err:%s", host.c_str());
		rsp["code"] = 1;
		rsp["msg"] = "connect err";
		prom->set_value(rsp.dump());
		return;
	}

	string nonce, created, digest;
	gen_digest(passwd, created, nonce, digest);

	unique_ptr<char[]> bufPtr = make_unique<char[]>(DEF_BUF_SIZE);

	ret = sprintf(bufPtr.get(),
	              "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
	              "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
	              "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
	              "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:"
	              "Username>%s</wsse:Username><wsse:Password "
	              "Type=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
	              "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
	              "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
	              "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
	              "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><GetPresets "
	              "xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>%s</ProfileToken></"
	              "GetPresets></s:Body></s:Envelope>",
	              user.c_str(), digest.c_str(), nonce.c_str(), created.c_str(), profile.c_str());

	MsHttpMsg req;
	string strReq;

	req.m_method = "POST";
	req.m_uri = uri;
	req.m_version = "HTTP/1.1";
	req.m_host.SetValue(host);
	req.m_connection.SetValue("close");
	req.m_contentType.SetValue("application/soap+xml; charset=utf-8");
	req.SetBody(bufPtr.get(), ret);
	req.Dump(strReq);

	ret = tcp_sock->Send(strReq.c_str(), strReq.size());
	if (ret < 0) {
		MS_LOG_ERROR("send err:%s", strReq.c_str());
		rsp["code"] = 1;
		rsp["msg"] = "send err";
		prom->set_value(rsp.dump());
		return;
	}

	int nrecv = 0;
	while (true) {
		ret = tcp_sock->Recv(bufPtr.get() + nrecv, DEF_BUF_SIZE - nrecv);
		if (ret < 1) {
			break;
		}

		nrecv += ret;
		bufPtr[nrecv] = '\0';
		char *p = strstr(bufPtr.get(), "Envelope>");
		if (p) {
			MS_LOG_DEBUG("presets:%s", bufPtr.get());
			// find the start of XML and move it to the beginning of buffer
			char *xmlStart = strstr(bufPtr.get(), "<?xml");
			if (xmlStart && xmlStart != bufPtr.get()) {
				size_t xmlLen = nrecv - (xmlStart - bufPtr.get());
				memmove(bufPtr.get(), xmlStart, xmlLen);
				nrecv = xmlLen;
				bufPtr[nrecv] = '\0';
			}

			auto presets = ParsePresetTokens(bufPtr.get());
			for (const auto &preset : presets) {
				json j;
				j["presetID"] = preset.token;
				j["name"] = preset.name;
				rsp["result"].emplace_back(j);
			}

			break;
		}
	}

	prom->set_value(rsp.dump());
}

void MsOnvifHandler::proc_probe(shared_ptr<MsEvent> evt) {
	auto xaddrsList = ParseXAddrs(m_bufPtr.get());

	if (xaddrsList.empty()) {
		MS_LOG_ERROR("buf err:%s", m_bufPtr.get());
		this->clear_evt(evt);
		return;
	}

	string devurl = xaddrsList[0];
	MS_LOG_DEBUG("device url:%s", devurl.c_str());

	string ip, uri;
	int port;

	if (parse_uri(devurl, ip, port, uri)) {
		MS_LOG_ERROR("devurl err:%s", devurl.c_str());
		this->clear_evt(evt);
		return;
	}

	if (m_dev->m_port == 0) {
		m_dev->m_port = port;
	}

	string nonce, created, digest;
	gen_digest(m_dev->m_pass, created, nonce, digest);

	shared_ptr<MsSocket> tcp_sock = make_shared<MsSocket>(AF_INET, SOCK_STREAM, 0);
	string host = ip + ":" + to_string(port);

	int ret = tcp_sock->Connect(ip, port);
	if (ret < 0) {
		MS_LOG_ERROR("connect err:%s", host.c_str());
		this->clear_evt(evt);
		return;
	}

	ret = sprintf(m_bufPtr.get(),
	              "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
	              "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
	              "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
	              "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:"
	              "Username>%s</wsse:Username><wsse:Password "
	              "Type=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
	              "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
	              "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
	              "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
	              "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><GetServices "
	              "xmlns=\"http://www.onvif.org/ver10/device/wsdl\"><IncludeCapability>false</"
	              "IncludeCapability></GetServices></s:Body></s:Envelope>",
	              m_dev->m_user.c_str(), digest.c_str(), nonce.c_str(), created.c_str());

	MsHttpMsg req;
	string strReq;

	req.m_method = "POST";
	req.m_uri = uri;
	req.m_version = "HTTP/1.1";
	req.m_host.SetValue(host);
	req.m_connection.SetValue("close");
	req.m_contentType.SetValue("application/soap+xml; charset=utf-8");
	req.SetBody(m_bufPtr.get(), ret);
	req.Dump(strReq);

	ret = tcp_sock->Send(strReq.c_str(), strReq.size());
	if (ret < 0) {
		MS_LOG_ERROR("send err:%s", strReq.c_str());
		this->clear_evt(evt);
		return;
	}

	MS_LOG_DEBUG("%s", strReq.c_str());

	shared_ptr<MsEvent> nevt =
	    make_shared<MsEvent>(tcp_sock, MS_FD_READ | MS_FD_CLOSE, shared_from_this());

	m_reactor->AddEvent(nevt);
	m_reactor->DelEvent(m_evt);
	m_evt = nevt;
	m_stage = STAGE_GET_SERVICES;
	m_nrecv = 0;
}

void MsOnvifHandler::proc_get_services(shared_ptr<MsEvent> evt) {

	auto services = ParseGetServicesResponse(m_bufPtr.get());
	if (services.media.xaddr.empty() && services.media2.xaddr.empty()) {
		MS_LOG_ERROR("media service not found:%s", m_bufPtr.get());
		this->clear_evt(evt);
		return;
	}

	m_mediaurl = services.media.xaddr.empty() ? services.media2.xaddr : services.media.xaddr;
	MS_LOG_DEBUG("media url:%s", m_mediaurl.c_str());

	if (!services.ptz.xaddr.empty()) {
		m_ptzurl = services.ptz.xaddr;
		MS_LOG_DEBUG("ptz url:%s", m_ptzurl.c_str());
	}

	string ip, uri;
	int port;
	if (parse_uri(m_mediaurl, ip, port, uri)) {
		MS_LOG_ERROR("media url err:%s", m_mediaurl.c_str());
		this->clear_evt(evt);
		return;
	}

	string nonce, created, digest;
	gen_digest(m_dev->m_pass, created, nonce, digest);

	shared_ptr<MsSocket> tcp_sock = make_shared<MsSocket>(AF_INET, SOCK_STREAM, 0);
	string host = ip + ":" + to_string(port);

	int ret = tcp_sock->Connect(ip, port);
	if (ret < 0) {
		MS_LOG_ERROR("connect err:%s", host.c_str());
		this->clear_evt(evt);
		return;
	}

	ret = sprintf(m_bufPtr.get(),
	              "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
	              "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
	              "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
	              "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:"
	              "Username>%s</wsse:Username><wsse:Password "
	              "Type=\"http://docs.oasis-open.org/wss/2004/01/"
	              "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
	              "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
	              "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
	              "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
	              "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><GetProfiles "
	              "xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/></s:Body></s:Envelope>",
	              m_dev->m_user.c_str(), digest.c_str(), nonce.c_str(), created.c_str());

	MsHttpMsg req;
	string strReq;

	req.m_method = "POST";
	req.m_uri = uri;
	req.m_version = "HTTP/1.1";
	req.m_host.SetValue(host);
	req.m_connection.SetValue("close");
	req.m_contentType.SetValue("application/soap+xml; charset=utf-8");
	req.SetBody(m_bufPtr.get(), ret);
	req.Dump(strReq);

	ret = tcp_sock->Send(strReq.c_str(), strReq.size());
	if (ret < 0) {
		MS_LOG_ERROR("send err:%s", strReq.c_str());
		this->clear_evt(evt);
		return;
	}

	shared_ptr<MsEvent> nevt =
	    make_shared<MsEvent>(tcp_sock, MS_FD_READ | MS_FD_CLOSE, shared_from_this());

	m_reactor->AddEvent(nevt);
	m_reactor->DelEvent(m_evt);
	m_evt = nevt;
	m_stage = STAGE_GET_PROFILES;
	m_nrecv = 0;
}

void MsOnvifHandler::proc_get_profiles(shared_ptr<MsEvent> evt) {
	auto token = ParseFirstProfileToken(m_bufPtr.get());
	if (token.empty()) {
		MS_LOG_ERROR("profiles not found:%s", m_bufPtr.get());
		this->clear_evt(evt);
		return;
	}

	m_profile = token;

	MS_LOG_DEBUG("profile:%s", m_profile.c_str());

	string ip, uri;
	int port;
	if (parse_uri(m_mediaurl, ip, port, uri)) {
		MS_LOG_ERROR("media url err:%s", m_mediaurl.c_str());
		this->clear_evt(evt);
		return;
	}

	string nonce, created, digest;
	gen_digest(m_dev->m_pass, created, nonce, digest);

	shared_ptr<MsSocket> tcp_sock = make_shared<MsSocket>(AF_INET, SOCK_STREAM, 0);
	string host = ip + ":" + to_string(port);

	int ret = tcp_sock->Connect(ip, port);
	if (ret < 0) {
		MS_LOG_ERROR("connect err:%s", host.c_str());
		this->clear_evt(evt);
		return;
	}

	ret = sprintf(
	    m_bufPtr.get(),
	    "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
	    "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header><wsse:Security "
	    "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
	    "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
	    "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
	    "oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:Username>%s</"
	    "wsse:Username><wsse:Password "
	    "Type=\"http://docs.oasis-open.org/wss/2004/01/"
	    "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</"
	    "wsse:Password><wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></"
	    "wsse:UsernameToken></wsse:Security></s:Header><s:Body "
	    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
	    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"><GetStreamUri "
	    "xmlns=\"http://www.onvif.org/ver10/media/wsdl\"><StreamSetup><Stream "
	    "xmlns=\"http://www.onvif.org/ver10/schema\">RTP-Unicast</Stream><Transport "
	    "xmlns=\"http://www.onvif.org/ver10/schema\"><Protocol>UDP</Protocol></Transport></"
	    "StreamSetup><ProfileToken>%s</ProfileToken></GetStreamUri></s:Body></s:Envelope>",
	    m_dev->m_user.c_str(), digest.c_str(), nonce.c_str(), created.c_str(), m_profile.c_str());

	MsHttpMsg req;
	string strReq;

	req.m_method = "POST";
	req.m_uri = uri;
	req.m_version = "HTTP/1.1";
	req.m_host.SetValue(host);
	req.m_connection.SetValue("close");
	req.m_contentType.SetValue("application/soap+xml; charset=utf-8");
	req.SetBody(m_bufPtr.get(), ret);
	req.Dump(strReq);

	ret = tcp_sock->Send(strReq.c_str(), strReq.size());
	if (ret < 0) {
		MS_LOG_ERROR("send err:%s", strReq.c_str());
		this->clear_evt(evt);
		return;
	}

	shared_ptr<MsEvent> nevt =
	    make_shared<MsEvent>(tcp_sock, MS_FD_READ | MS_FD_CLOSE, shared_from_this());

	m_reactor->AddEvent(nevt);
	m_reactor->DelEvent(m_evt);
	m_evt = nevt;
	m_stage = STAGE_GET_STREAM_URI;
	m_nrecv = 0;
}

void MsOnvifHandler::proc_get_stream_uri(shared_ptr<MsEvent> evt) {
	auto rtsp = ParseStreamUri(m_bufPtr.get());
	if (rtsp.empty()) {
		MS_LOG_ERROR("rtsp url not found:%s", m_bufPtr.get());
		this->clear_evt(evt);
		return;
	}

	m_rtsp = rtsp;

	MS_LOG_DEBUG("rtsp url:%s", m_rtsp.c_str());

	m_reactor->DelEvent(m_evt);
	m_evt.reset();

	string rtspurl =
	    "rtsp://" + m_dev->m_user + ":" + m_dev->m_pass + "@" + m_rtsp.substr(strlen("rtsp://"));

	ModDev mod;
	mod.m_url = rtspurl;
	mod.m_onvifptzurl = m_ptzurl;
	mod.m_onvifprofile = m_profile;

	MsDevMgr::Instance()->ModifyDevice(m_dev->m_deviceID, mod);

	MsMsg xm;
	xm.m_msgID = MS_ONVIF_PROBE_FINISH;
	xm.m_strVal = m_dev->m_deviceID;
	m_reactor->EnqueMsg(xm);
}

void MsOnvifHandler::clear_evt(shared_ptr<MsEvent> evt) {
	if (m_evt.get()) {
		if (evt->GetSocket()->GetFd() == m_evt->GetSocket()->GetFd()) {
			m_evt.reset();
		}
	}

	m_reactor->DelEvent(evt);
}

int MsOnvifHandler::parse_uri(string &url, string &ip, int &port, string &uri) {
	if (0 != memcmp(url.c_str(), "http://", strlen("http://"))) {
		MS_LOG_ERROR("err url:%s", url.c_str());
		return -1;
	}

	string tt = url.substr(strlen("http://"));
	size_t p = tt.find_first_of('/');
	if (p == string::npos) {
		MS_LOG_ERROR("err url:%s", url.c_str());
		return -1;
	}

	uri = tt.substr(p);
	tt = tt.substr(0, p);

	p = tt.find_first_of(':');
	if (p == string::npos) {
		port = 80;
		ip = tt;
	} else {
		ip = tt.substr(0, p);
		port = stoi(tt.substr(p + 1));
	}

	MS_LOG_DEBUG("ip:%s port:%d uri:%s", ip.c_str(), port, uri.c_str());
	return 0;
}

void MsOnvifHandler::gen_digest(string &passwd, string &created, string &nonce, string &digest) {
	string nx1 = GenRandStr(20);

	nonce = EncodeBase64((const unsigned char *)nx1.c_str(), nx1.size());
	created = GmtTimeToStr(time(nullptr));
	created += "Z";

	string cc = nx1 + created + passwd;

	unsigned char xxbuf[20];
	sha1::calc(cc.c_str(), cc.size(), xxbuf);
	digest = EncodeBase64(&xxbuf[0], 20);
}
