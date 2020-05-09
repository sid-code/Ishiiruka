#include "SlippiMatchmaking.h"
#include "Common/ENetUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include <string>

class MmMessageType
{
  public:
	static std::string CREATE_TICKET;
	static std::string CREATE_TICKET_RESP;
	static std::string DELETE_TICKET;
	static std::string DELETE_TICKET_RESP;
	static std::string GET_TICKET;
	static std::string GET_TICKET_RESP;
};

std::string MmMessageType::CREATE_TICKET = "create-ticket";
std::string MmMessageType::CREATE_TICKET_RESP = "create-ticket-resp";
std::string MmMessageType::DELETE_TICKET = "delete-ticket";
std::string MmMessageType::DELETE_TICKET_RESP = "delete-ticket-resp";
std::string MmMessageType::GET_TICKET = "get-ticket";
std::string MmMessageType::GET_TICKET_RESP = "get-ticket-resp";

SlippiMatchmaking::SlippiMatchmaking(SlippiUser *user)
{
	m_user = user;
	m_state = ProcessState::IDLE;

  m_client = nullptr;
	m_server = nullptr;
}

SlippiMatchmaking::~SlippiMatchmaking()
{
	m_state = ProcessState::ERROR_ENCOUNTERED;

	if (m_matchmakeThread.joinable())
		m_matchmakeThread.join();

  terminateMmConnection();
}

void SlippiMatchmaking::FindMatch()
{
	m_hostPort = 51000;

	// We are explicitly setting the client address because we are trying to utilize our connection
	// to the matchmaking service in order to hole punch. This port will end up being the port
	// we listen on when we start our server
	ENetAddress clientAddr;
	clientAddr.host = ENET_HOST_ANY;
	clientAddr.port = m_hostPort;

	m_client = enet_host_create(&clientAddr, 1, 3, 0, 0);

	if (m_client == nullptr)
	{
		// Failed to create client
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	ENetAddress addr;
	enet_address_set_host(&addr, MM_HOST.c_str());
	addr.port = MM_PORT;

	m_server = enet_host_connect(m_client, &addr, 3, 0);

	if (m_server == nullptr)
	{
		// Failed to connect to server
		m_state = ProcessState::ERROR_ENCOUNTERED;
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Failed to start connection to mm server...");
		return;
	}

	isMmConnected = false;

	m_state = ProcessState::INITIALIZING;
	m_matchmakeThread = std::thread(&SlippiMatchmaking::MatchmakeThread, this);
}

SlippiMatchmaking::ProcessState SlippiMatchmaking::GetMatchmakeState()
{
	return m_state;
}

bool SlippiMatchmaking::IsSearching()
{
	return searchingStates.count(m_state) != 0;
}

std::unique_ptr<SlippiNetplayClient> SlippiMatchmaking::GetNetplayClient()
{
	return std::move(m_netplayClient);
}

void SlippiMatchmaking::MatchmakeThread()
{
	while (IsSearching())
	{
		switch (m_state)
		{
		case ProcessState::INITIALIZING:
			startMatchmaking();
			break;
		case ProcessState::MATCHMAKING:
			handleMatchmaking();
			break;
		case ProcessState::OPPONENT_CONNECTING:
			handleConnecting();
			break;
		}
	}

	// TODO: Clean up ENET connections
}

void SlippiMatchmaking::sendMessage(json msg)
{
	enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
	u8 channelId = 0;

	sf::Packet packet;
	packet << msg.dump();

	ENetPacket *epac = enet_packet_create(packet.getData(), packet.getDataSize(), flags);
	enet_peer_send(m_server, channelId, epac);
}

int SlippiMatchmaking::receiveMessage(json &msg, int maxAttempts)
{
	for (int i = 0; i < maxAttempts; i++)
	{
		ENetEvent netEvent;
		int net = enet_host_service(m_client, &netEvent, 250);
		if (net <= 0)
			continue;

		switch (netEvent.type)
		{
		case ENET_EVENT_TYPE_RECEIVE:
		{
			sf::Packet rpac;
			rpac.append(netEvent.packet->data, netEvent.packet->dataLength);

			std::string str;
			rpac >> str;

			msg = json::parse(str);

			enet_packet_destroy(netEvent.packet);
			return 0;
		}
		case ENET_EVENT_TYPE_DISCONNECT:
			// TODO: Handle
			break;
		}
	}

	return -1;
}

void SlippiMatchmaking::disconnectFromServer() {
	if (m_server)
		enet_peer_disconnect(m_server, 0);
	else
		return;

  ENetEvent netEvent;
	while (enet_host_service(m_client, &netEvent, 3000) > 0)
	{
		switch (netEvent.type)
		{
		case ENET_EVENT_TYPE_RECEIVE:
			enet_packet_destroy(netEvent.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			m_server = nullptr;
			return;
		default:
			break;
		}
	}

	// didn't disconnect gracefully force disconnect
	enet_peer_reset(m_server);
	m_server = nullptr;
}

void SlippiMatchmaking::terminateMmConnection()
{
  // Disconnect from server
	disconnectFromServer();

  // Destroy client
  if (m_client)
  {
	  enet_host_destroy(m_client);
	  m_client = nullptr;
  }
}

void SlippiMatchmaking::startMatchmaking()
{
	// Before we can request a ticket, we must wait for connection to be successful
	int connectAttemptCount = 0;
	while (!isMmConnected)
	{
		ENetEvent netEvent;
		int net = enet_host_service(m_client, &netEvent, 1000);
		if (net <= 0 || netEvent.type != ENET_EVENT_TYPE_CONNECT)
		{
			// Not yet connected, will retry
			connectAttemptCount++;
			if (connectAttemptCount >= 30)
			{
				ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Failed to connect to mm server...");
				m_state = ProcessState::ERROR_ENCOUNTERED;
				return;
			}

			continue;
		}

		m_client->intercept = ENetUtil::InterceptCallback;
		isMmConnected = true;
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Connected to mm server...");
	}

	ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Trying to find match...");

	// Reset variables for
	m_ticketId.clear();

	if (!m_user->IsLoggedIn())
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Must be logged in to queue");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	auto userInfo = m_user->GetUserInfo();

	// Send message to server to create ticket
	json request;
	request["type"] = MmMessageType::CREATE_TICKET;
	request["user"] = {{"uid", userInfo.uid}, {"playKey", userInfo.playKey}};
	sendMessage(request);

	// Get response from server
	json response;
	int rcvRes = receiveMessage(response, 20);
	if (rcvRes != 0)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Did not receive response from server for create ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	std::string respType = response["type"];
	if (respType != MmMessageType::CREATE_TICKET_RESP)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Received incorrect response for create ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	std::string err = response.value("error", "");
	if (err.length() > 0)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Received error from server for create ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	// Process ticket ID
	std::string ticketId = response["ticketId"];
	m_ticketId.insert(m_ticketId.end(), ticketId.begin(), ticketId.end());

	m_state = ProcessState::MATCHMAKING;
	ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Request ticket success: %s", m_ticketId.c_str());
}

void SlippiMatchmaking::handleMatchmaking()
{
	Common::SleepCurrentThread(1000);
	if (m_state != ProcessState::MATCHMAKING)
	{
		// If the destructor was called during the sleep, state might have changed
		return;
	}

	// Send message to server to get ticket
	json getReq;
	getReq["type"] = MmMessageType::GET_TICKET;
	getReq["ticketId"] = m_ticketId;
	sendMessage(getReq);

	// Get response from server
	json getResp;
	int rcvRes = receiveMessage(getResp, 20);
	if (rcvRes != 0)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Did not receive response from server for get ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	std::string respType = getResp["type"];
	if (respType != MmMessageType::GET_TICKET_RESP)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Received incorrect response for get ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	std::string err = getResp.value("error", "");
	if (err.length() > 0)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Received error from server for get ticket");
		m_state = ProcessState::ERROR_ENCOUNTERED;
		return;
	}

	// Handle case where ticket has yet to be assigned
	if (!getResp.value("isAssigned", false))
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] No assignment found yet");
		return;
	}

	// Delete ticket now that we've been assigned
	// Send message to server to delete ticket
	json deleteReq;
	deleteReq["type"] = MmMessageType::DELETE_TICKET;
	deleteReq["ticketId"] = m_ticketId;
	sendMessage(deleteReq);

	// Get response from server
	json deleteResp;
	rcvRes = receiveMessage(deleteResp, 20);
	if (rcvRes != 0)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Did not receive response from server for delete ticket");
	}

	std::string deleteRespType = deleteResp.value("type", "");
	if (deleteRespType != MmMessageType::DELETE_TICKET_RESP)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Received incorrect response for delete ticket");
	}

	err = deleteResp.value("error", "");
	if (err.length() > 0)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Received error from server for delete ticket");
	}

	m_netplayClient = nullptr;
	m_oppIp = getResp.value("oppAddress", "");
	m_isHost = getResp.value("isHost", false);

	// Disconnect and destroy enet client to mm server
	terminateMmConnection();

	m_state = ProcessState::OPPONENT_CONNECTING;
	ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Opponent found. IP: %s, isHost: %s", m_oppIp.c_str(),
	          m_isHost ? "true" : "false");
}

void SlippiMatchmaking::handleConnecting()
{
	std::vector<std::string> ipParts;
	SplitString(m_oppIp, ':', ipParts);

	if (!m_netplayClient)
	{
		int port = m_isHost ? m_hostPort : std::stoi(ipParts[1]);
		m_netplayClient = std::make_unique<SlippiNetplayClient>(ipParts[0], port, m_isHost);
	}

	auto status = m_netplayClient->GetSlippiConnectStatus();
	if (status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_INITIATED)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Matchmaking] Connection not yet successful");
		Common::SleepCurrentThread(1000);
		return;
	}
	else if (status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_FAILED)
	{
		// Return to the start to get a new ticket to find someone else we can hopefully connect with
		m_netplayClient = nullptr;
		m_state = ProcessState::INITIALIZING;
		return;
	}

	// Connection success, our work is done
	m_state = ProcessState::CONNECTION_SUCCESS;
}
