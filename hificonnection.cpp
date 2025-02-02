#include <random>

#include "hificonnection.h"

HifiConnection::HifiConnection(QWebSocket * s)
{
    username = "";
    password = "";
    keypair_generator = new RSAKeypairGenerator();
    waiting_for_keypair = false;
    token = "";

    domain_name = "";
    domain_place_name = "";
    domain_id = QUuid();
    finished_domain_id_request = false;
    stun_server_hostname = "stun.highfidelity.io";
    stun_server_address = QHostAddress();
    stun_server_port = 3478;
    ice_server_hostname = "ice.highfidelity.com"; //"dev-ice.highfidelity.com";

    qDebug() << "HifiConnection::HifiConnection() - Synchronously looking up IP address for hostname" << stun_server_hostname;
    QHostInfo result_stun = QHostInfo::fromName(stun_server_hostname);
    HandleLookupResult(result_stun, "stun");
    //qDebug() << "HifiConnection::HifiConnection() - STUN server IP address: " << stun_server_hostname;

    qDebug() << "HifiConnection::HifiConnection() - Synchronously looking up IP address for hostname" << ice_server_hostname;
    QHostInfo result_ice = QHostInfo::fromName(ice_server_hostname);
    HandleLookupResult(result_ice, "ice");
    //qDebug() << "HifiConnection::HifiConnection() - ICE server IP address: " << ice_server_hostname;

    ice_server_address = Utils::GetDefaultIceServerAddress();
    ice_server_port = Utils::GetDefaultIceServerPort();

    has_tcp_checked_local_socket = false;
    UpdateLocalSocket();

    connect(this, SIGNAL(WebRTCConnectionReady()), this, SLOT(StartStun()));
    connect(this, SIGNAL(StunFinished()), this, SLOT(StartIce()));
    connect(this, SIGNAL(IceFinished()), this, SLOT(StartDomainConnect()));

    ice_client_id = QUuid::createUuid();

    started_domain_connect = false;

    owner_type = NodeType::Agent;
    node_types_of_interest = NodeSet() << NodeType::AudioMixer << NodeType::AvatarMixer << NodeType::EntityServer << NodeType::AssetServer << NodeType::MessagesMixer << NodeType::EntityScriptServer;

    domain_connected = false;

    asset_server = nullptr;
    audio_mixer = nullptr;
    avatar_mixer = nullptr;
    messages_mixer = nullptr;
    entity_server = nullptr;
    entity_script_server = nullptr;
    data_channel = nullptr;

    stun_response_timer = new QTimer { this };
    connect(stun_response_timer, &QTimer::timeout, this, &HifiConnection::SendStunRequest);
    stun_response_timer->setInterval(HIFI_INITIAL_UPDATE_INTERVAL_MSEC); // 250ms, Qt::CoarseTimer acceptable

    ice_response_timer = new QTimer { this };
    connect(ice_response_timer, &QTimer::timeout, this, &HifiConnection::SendIceRequest);
    ice_response_timer->setInterval(HIFI_INITIAL_UPDATE_INTERVAL_MSEC); // 250ms, Qt::CoarseTimer acceptable

    hifi_response_timer = new QTimer { this };
    connect(hifi_response_timer, &QTimer::timeout, this, &HifiConnection::SendDomainCheckIn);
    hifi_response_timer->setInterval(HIFI_INITIAL_UPDATE_INTERVAL_MSEC); // 250ms, Qt::CoarseTimer acceptable

    qDebug() << "HifiConnection::Connect() - New client" << s << ice_client_id << s->peerAddress() << s->peerPort();
    client_socket = s;

    connect(client_socket, &QWebSocket::textMessageReceived, this, &HifiConnection::ClientMessageReceived);

    started_hifi_connect = false;
    hifi_socket = new QUdpSocket(this);
    connect(hifi_socket, SIGNAL(readyRead()), this, SLOT(ParseHifiResponse()));

    QJsonObject connected_object;
    connected_object.insert("type", QJsonValue::fromVariant("connected"));
    connected_object.insert("id", QJsonValue::fromVariant(ice_client_id.toString()));
    QJsonDocument connectedDoc(connected_object);
    client_socket->sendTextMessage(QString::fromStdString(connectedDoc.toJson(QJsonDocument::Compact).toStdString()));

    client_timestamp = Utils::GetTimestamp();
    server_timestamp = client_timestamp;

    timeout_timer = new QTimer { this };
    connect(timeout_timer, &QTimer::timeout, this, &HifiConnection::Timeout);
    timeout_timer->setInterval(HIFI_TIMEOUT_MSEC/4);
    timeout_timer->start();
}

HifiConnection::~HifiConnection()
{

}

void HifiConnection::HandleLookupResult(const QHostInfo& hostInfo, QString addr_type)
{
    if (hostInfo.error() != QHostInfo::NoError) {
        qDebug() << "Task::handleLookupResult() - Lookup failed for" << hostInfo.lookupId() << ":" << hostInfo.errorString();
    } else {
        for (int i = 0; i < hostInfo.addresses().size(); i++) {
            // just take the first IPv4 address
            QHostAddress address = hostInfo.addresses()[i];
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {

                if (addr_type == "stun") stun_server_address = address;
                else if (addr_type == "ice") ice_server_address = address;

                qDebug() << "Task::handleLookupResult() - QHostInfo lookup result for"
                    << hostInfo.hostName() << "with lookup ID" << hostInfo.lookupId() << "is" << address.toString();
                break;
            }
        }
    }
}

void HifiConnection::UpdateLocalSocket()
{
    // attempt to use Google's DNS to confirm that local IP
    const QHostAddress RELIABLE_LOCAL_IP_CHECK_HOST = QHostAddress { "8.8.8.8" };
    const int RELIABLE_LOCAL_IP_CHECK_PORT = 53;

    QTcpSocket* localIPTestSocket = new QTcpSocket;

    connect(localIPTestSocket, &QTcpSocket::connected, this, &HifiConnection::ConnectedForLocalSocketTest);
    connect(localIPTestSocket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(ErrorTestingLocalSocket()));

    // attempt to connect to our reliable host
    localIPTestSocket->connectToHost(RELIABLE_LOCAL_IP_CHECK_HOST, RELIABLE_LOCAL_IP_CHECK_PORT);
}

void HifiConnection::ConnectedForLocalSocketTest()
{
    auto local_ip_test_socket = qobject_cast<QTcpSocket*>(sender());

    if (local_ip_test_socket) {
        auto local_host_address = local_ip_test_socket->localAddress();

        if (local_host_address.protocol() == QAbstractSocket::IPv4Protocol) {
            local_address = local_host_address;

            //qDebug() << "HifiConnection::connectedForLocalSocketTest() - Local address: " << local_address;

            has_tcp_checked_local_socket = true;
        }

        local_ip_test_socket->deleteLater();
    }
}

void HifiConnection::ErrorTestingLocalSocket()
{
    auto local_ip_test_socket = qobject_cast<QTcpSocket*>(sender());

    if (local_ip_test_socket) {

        // error connecting to the test socket - if we've never set our local socket using this test socket
        // then use our possibly updated guessed local address as fallback
        if (!has_tcp_checked_local_socket) {
            local_address = GetGuessedLocalAddress();

            //qDebug() << "HifiConnection::errorTestingLocalSocket() - Local address: " << local_address;

            has_tcp_checked_local_socket = true;
        }

        local_ip_test_socket->deleteLater();
    }
}

QHostAddress HifiConnection::GetGuessedLocalAddress()
{
    QHostAddress address;

    for (int i= 0; i < QNetworkInterface::allInterfaces().size(); i++) {
        QNetworkInterface network_interface = QNetworkInterface::allInterfaces()[i];
        if (network_interface.flags() & QNetworkInterface::IsUp
            && network_interface.flags() & QNetworkInterface::IsRunning
            && network_interface.flags() & ~QNetworkInterface::IsLoopBack) {
            // we've decided that this is the active NIC
            // enumerate it's addresses to grab the IPv4 address
            for (int j = 0; network_interface.addressEntries().size(); j++) {
                // make sure it's an IPv4 address that isn't the loopback
                QNetworkAddressEntry entry = network_interface.addressEntries()[j];
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback()) {

                    // set our localAddress and break out
                    address = entry.ip();
                    //qDebug() << "HifiConnection::getGuessedLocalAddress() - " << address;
                    break;
                }
            }
        }

        if (!address.isNull()) {
            break;
        }
    }

    has_tcp_checked_local_socket = true;

    // return the looked up local address
    return address;
}

void HifiConnection::Stop()
{
    if (asset_server) {
        delete asset_server;
        asset_server = nullptr;
    }
    if (audio_mixer) {
        delete audio_mixer;
        audio_mixer = nullptr;
    }
    if (messages_mixer) {
        delete messages_mixer;
        messages_mixer = nullptr;
    }
    if (avatar_mixer) {
        delete avatar_mixer;
        avatar_mixer = nullptr;
    }
    if (entity_script_server) {
        delete entity_script_server;
        entity_script_server = nullptr;
    }
    if (entity_server) {
        delete entity_server;
        entity_server = nullptr;
    }

    if (data_channel) {
        ClearDataChannel();
    }

    if (timeout_timer) {
        delete timeout_timer;
        timeout_timer = nullptr;
    }

    if (ice_response_timer) {
        delete ice_response_timer;
        ice_response_timer = nullptr;
    }

    if (stun_response_timer) {
        delete stun_response_timer;
        stun_response_timer = nullptr;
    }

    if (hifi_response_timer) {
        delete hifi_response_timer;
        hifi_response_timer = nullptr;
    }

    if (client_socket) {
        delete client_socket;
        client_socket = nullptr;
    }

    if (hifi_socket) {
        delete hifi_socket;
        hifi_socket = nullptr;
    }
}

void HifiConnection::DomainRequestFinished()
{
    QNetworkReply *domain_reply = qobject_cast<QNetworkReply *>(sender());
    if (domain_reply) {
        QByteArray domain_reply_contents;
        if (domain_reply->error() == QNetworkReply::NoError && domain_reply->isOpen()) {
            domain_reply_contents += domain_reply->readAll();

            //qDebug() << domain_reply_contents;

            QJsonDocument doc;
            doc = QJsonDocument::fromJson(domain_reply_contents);
            QJsonObject obj = doc.object();
            QJsonObject data = obj["data"].toObject();
            QJsonObject place = data["place"].toObject();
            QJsonObject domain = place["domain"].toObject();
            domain_id = QUuid(domain["id"].toString());
            domain_place_name = domain["default_place_name"].toString();

            if (domain.contains("ice_server_address")) {
                ice_server_address = QHostAddress(domain["ice_server_address"].toString());
            }
        }
        domain_reply->close();
        domain_reply->deleteLater();
    }

    qDebug() << "HifiConnection::domainRequestFinished() - Domain name" << domain_name;
    qDebug() << "HifiConnection::domainRequestFinished() - Domain place name" << domain_place_name;
    qDebug() << "HifiConnection::domainRequestFinished() - Domain ID" << domain_id;

    finished_domain_id_request = true;
    if (data_channel && finished_domain_id_request && !started_hifi_connect) {
        started_hifi_connect = true;
        Q_EMIT WebRTCConnectionReady();
    }
}

void HifiConnection::RequestAccessTokenFinished() {
    QNetworkReply* reply = reinterpret_cast<QNetworkReply*>(sender());

    QJsonDocument response = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject& root_object = response.object();

    if (!root_object.contains("error")) {
        // construct an OAuthAccessToken from the json object

        if (!root_object.contains("access_token") || !root_object.contains("expires_in")
            || !root_object.contains("token_type")) {
            qDebug() << "Received a response for password grant that is missing one or more expected values.";
        } else {
            // clear the path from the response URL so we have the right root URL for this access token
            QUrl url = reply->url();
            url.setPath("");

            qDebug() << "Storing an account with access-token for" << url.toString();

            token = root_object["access_token"].toString();
            refreshToken = root_object["refresh_token"].toString();
            expiryTimestamp = QDateTime::currentMSecsSinceEpoch() + (root_object["expires_in"].toDouble() * 1000);
            tokenType = root_object["token_type"].toString();
        }
    } else {
        qDebug() <<  "Error in response for password grant -" << root_object["error_description"].toString();
    }
}

void HifiConnection::RequestAccessTokenError(QNetworkReply::NetworkError error) {
    qDebug() << "HifiConnection: failed to fetch access token - " << error;
}

void HifiConnection::StartStun()
{
    // Register Domain Server DC callbacks here
    std::function<void(std::string)> onErrorCallback = [this](std::string message) {
        qDebug() << "HifiConnection::onError() - Data channel error" << QString::fromStdString(message);
    };
    data_channel->SetOnErrorCallback(onErrorCallback);

    std::function<void(rtcdcpp::ChunkPtr)> onBinaryMessageCallback = [this](rtcdcpp::ChunkPtr message) {
        this->client_timestamp = Utils::GetTimestamp();

        NodeType_t server = (NodeType_t) message->Data()[0];
        QByteArray packet = QByteArray((char *) (message->Data() + sizeof(NodeType_t)), message->Length() - 1);
        //qDebug() << "HifiConnection::onMessage() - " << (char) server << packet << packet.size();

        if (server == NodeType::DomainServer) {
            //qDebug() << "domain";
            bool is_control_packet = *reinterpret_cast<uint32_t*>(packet.data()) & CONTROL_BIT_MASK;
            if (is_control_packet) {
                this->SendServerMessage(packet, domain_public_address, domain_public_port);
            }
            else {
                std::unique_ptr<Packet> response_packet = Packet::FromReceivedPacket(packet.data(), (qint64) packet.size());// check if this was a control packet or a data packet
                if (response_packet->GetType() == PacketType::ProxiedICEPing) {
                    uint8_t ping_type = 2; //Default to public
                    response_packet->read(reinterpret_cast<char*>(&ping_type), sizeof(uint8_t));
                    //qDebug() << "proxiediceping" << ping_type;
                    SendIcePing(response_packet->GetSequenceNumber(), ping_type);
                }
                else if (response_packet->GetType() == PacketType::ProxiedICEPingReply) {
                    uint8_t ping_type = 2; //Default to public
                    response_packet->read(reinterpret_cast<char*>(&ping_type), sizeof(uint8_t));
                    //qDebug() << "proxiedicepingreply" << ping_type;
                    SendIcePingReply(response_packet->GetSequenceNumber(), ping_type);
                }
                else if (response_packet->GetType() == PacketType::ProxiedDomainListRequest) {
                    //qDebug() << "proxieddomainlistrequest";
                    SendDomainCheckInRequest(response_packet->GetSequenceNumber());
                }
                else {
                    this->SendServerMessage(packet, domain_public_address, domain_public_port);
                }
            }
        }
        else if (server == NodeType::AssetServer) {
            //qDebug() << "asset";
            if (this->asset_server) SendServerMessage(packet, asset_server->GetPublicAddress(), asset_server->GetPublicPort());
        }
        else if (server == NodeType::AudioMixer) {
            //qDebug() << "audio";
            if (this->audio_mixer) SendServerMessage(packet, audio_mixer->GetPublicAddress(), audio_mixer->GetPublicPort());
        }
        else if (server == NodeType::AvatarMixer) {
            //qDebug() << "avatar";
            if (this->avatar_mixer) SendServerMessage(packet, avatar_mixer->GetPublicAddress(), avatar_mixer->GetPublicPort());
        }
        else if (server == NodeType::MessagesMixer) {
            //qDebug() << "messages";
            if (this->messages_mixer) SendServerMessage(packet, messages_mixer->GetPublicAddress(), messages_mixer->GetPublicPort());
        }
        else if (server == NodeType::EntityServer) {
            //qDebug() << "entity";
            if (this->entity_server) SendServerMessage(packet, entity_server->GetPublicAddress(), entity_server->GetPublicPort());
        }
        else if (server == NodeType::EntityScriptServer) {
            //qDebug() << "entityscript";
            if (this->entity_script_server) SendServerMessage(packet, entity_script_server->GetPublicAddress(), entity_script_server->GetPublicPort());
        }
    };
    data_channel->SetOnBinaryMsgCallback(onBinaryMessageCallback);

    std::function<void()> onClosed = [this]() {
        qDebug() << "HifiConnection::onClosed() - Domain Server data channel closed";
        ClearDataChannel();
        Q_EMIT Disconnected();
    };
    data_channel->SetOnClosedCallback(onClosed);

    num_requests = 0;
    has_completed_current_request = false;

    stun_response_timer->start();
}

void HifiConnection::Timeout()
{
    quint64 timestamp = Utils::GetTimestamp();
    //qDebug() << "Timeout" << timestamp / 1000 << client_timestamp / 1000 << server_timestamp / 1000;

    if (timestamp > client_timestamp && (timestamp - client_timestamp) / 1000 > HIFI_TIMEOUT_MSEC) { // Convert to millisecond
        qDebug() << "HifiConnection::Timeout() - Client connection timed out. Disconnecting...";

        if (timeout_timer) {
            delete timeout_timer;
            timeout_timer = nullptr;
        }

        Q_EMIT Disconnected();
        return;
    }
    else if (timestamp > server_timestamp && (timestamp - server_timestamp) / 1000000 > HIFI_TIMEOUT_MSEC) { // Convert to millisecond
        qDebug() << "HifiConnection::Timeout() - Server connection timed out. Disconnecting...";

        if (timeout_timer) {
            delete timeout_timer;
            timeout_timer = nullptr;
        }

        Q_EMIT Disconnected();
        return;
    }
}

void HifiConnection::StartIce()
{
    num_requests = 0;
    has_completed_current_request = false;

    ice_response_timer->start();
}

void HifiConnection::StartDomainConnect()
{
    connect(hifi_socket, SIGNAL(disconnected()), this, SLOT(ServerDisconnected()));

    num_requests = 0;
    has_completed_current_request = false;

    hifi_response_timer->start();
}

void HifiConnection::ParseHifiResponse()
{
    while (hifi_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(hifi_socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 sender_port;

        hifi_socket->readDatagram(datagram.data(), datagram.size(), &sender, &sender_port);

        server_timestamp = Utils::GetTimestamp();

        //Stun Server response;
        if (sender.toIPv4Address() == stun_server_address.toIPv4Address() && sender_port == stun_server_port) {
            //qDebug() << "HifiConnection::ParseHifiResponse() - read packet from " << sender << ":" << sender_port << " of size " << datagram.size() << " bytes";

            // check the cookie to make sure this is actually a STUN response
            // and read the first attribute and make sure it is a XOR_MAPPED_ADDRESS
            const int NUM_BYTES_MESSAGE_TYPE_AND_LENGTH = 4;
            const uint16_t XOR_MAPPED_ADDRESS_TYPE = htons(0x0020);

            const uint32_t RFC_5389_MAGIC_COOKIE_NETWORK_ORDER = htonl(RFC_5389_MAGIC_COOKIE);

            int attribute_start_index = NUM_BYTES_STUN_HEADER;
            if (memcmp(datagram.data() + NUM_BYTES_MESSAGE_TYPE_AND_LENGTH,
                       &RFC_5389_MAGIC_COOKIE_NETWORK_ORDER,
                       sizeof(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER)) != 0) {
                qDebug() << "HifiConnection::ParseHifiResponse() - STUN response cannot be parsed, magic cookie is invalid";
                Q_EMIT Disconnected();
                return;
            }

            // enumerate the attributes to find XOR_MAPPED_ADDRESS_TYPE
            while (attribute_start_index < datagram.size()) {
                if (memcmp(datagram.data() + attribute_start_index, &XOR_MAPPED_ADDRESS_TYPE, sizeof(XOR_MAPPED_ADDRESS_TYPE)) == 0) {
                    const int NUM_BYTES_STUN_ATTR_TYPE_AND_LENGTH = 4;
                    const int NUM_BYTES_FAMILY_ALIGN = 1;
                    const uint8_t IPV4_FAMILY_NETWORK_ORDER = htons(0x01) >> 8;

                    int byte_index = attribute_start_index + NUM_BYTES_STUN_ATTR_TYPE_AND_LENGTH + NUM_BYTES_FAMILY_ALIGN;

                    uint8_t address_family = 0;
                    memcpy(&address_family, datagram.data() + byte_index, sizeof(address_family));

                    byte_index += sizeof(address_family);

                    if (address_family == IPV4_FAMILY_NETWORK_ORDER) {
                        // grab the X-Port
                        uint16_t xor_mapped_port = 0;
                        memcpy(&xor_mapped_port, datagram.data() + byte_index, sizeof(xor_mapped_port));

                        public_port = ntohs(xor_mapped_port) ^ (ntohl(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER) >> 16);

                        byte_index += sizeof(xor_mapped_port);

                        // grab the X-Address
                        uint32_t xor_mapped_address = 0;
                        memcpy(&xor_mapped_address, datagram.data() + byte_index, sizeof(xor_mapped_address));

                        uint32_t stun_address = ntohl(xor_mapped_address) ^ ntohl(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER);

                        // QHostAddress newPublicAddress(stun_address);
                        public_address = QHostAddress(stun_address);

                        qDebug() << "HifiConnection::ParseHifiResponse() - Public address: " << public_address;
                        qDebug() << "HifiConnection::ParseHifiResponse() - Public port: " << public_port;

                        local_port = hifi_socket->localPort();

                        qDebug() << "HifiConnection::ParseHifiResponse() - Local address: " << local_address;
                        qDebug() << "HifiConnection::ParseHifiResponse() - Local port: " << local_port;

                        has_completed_current_request = true;
                        stun_response_timer->stop();

                        SendClientMessageFromNode(NodeType::DomainServer, datagram);
                        Q_EMIT StunFinished();
                        break;
                    }
                }
                else {
                    // push forward attribute_start_index by the length of this attribute
                    const int NUM_BYTES_ATTRIBUTE_TYPE = 2;

                    uint16_t attribute_length = 0;
                    memcpy(&attribute_length, datagram.data() + attribute_start_index + NUM_BYTES_ATTRIBUTE_TYPE,
                           sizeof(attribute_length));
                    attribute_length = ntohs(attribute_length);

                    attribute_start_index += NUM_BYTES_MESSAGE_TYPE_AND_LENGTH + attribute_length;
                }
            }
            continue;
        }

        bool is_control_packet = *reinterpret_cast<uint32_t*>(datagram.data()) & CONTROL_BIT_MASK;
        if (!is_control_packet) {
            ParseDatagram(datagram);
        }

        Node * node = GetNodeFromAddress(sender, sender_port);
        if (node) {
            SendClientMessageFromNode(node->GetNodeType(), datagram);
        }
        else {
            SendClientMessageFromNode(NodeType::DomainServer, datagram);
        }
    }
}

void HifiConnection::ParseDatagram(QByteArray datagram)
{
    std::unique_ptr<Packet> response_packet = Packet::FromReceivedPacket(datagram.data(), (qint64) datagram.size());// check if this was a control packet or a data packet
    //qDebug() << "HifiConnection::ParseHifiResponse() - Packet type" << (int) response_packet->GetType();
    //ICE response
    if (response_packet->GetType() == PacketType::ICEServerPeerInformation)
    {
        QDataStream ice_response_stream(response_packet.get()->readAll());
        QUuid domain_uuid;
        ice_response_stream >> domain_uuid >> domain_public_address >> domain_public_port >> domain_local_address >> domain_local_port;

        if (domain_uuid != domain_id){
            qDebug() << "HifiConnection::ParseHifiResponse() - Error: Domain ID's do not match " << domain_uuid << domain_id;
            ice_response_timer->stop();
            Q_EMIT Disconnected();
        }

        qDebug() << "HifiConnection::ParseHifiResponse() - Domain ID: " << domain_uuid << "Domain Public Address: " << domain_public_address << "Domain Public Port: " << domain_public_port << "Domain Local Address: " << domain_local_address << "Domain Local Port: " << domain_local_port;

        has_completed_current_request = true;
        if (!started_domain_connect)
        {
            ice_response_timer->stop();
            Q_EMIT IceFinished();
        }
    }
    else if (response_packet->GetType() == PacketType::DomainServerConnectionToken) {
        num_requests = 0; // Reset number of requests so we can keep sending DomainConnectRequests

        QByteArray token(response_packet->readAll().constData(), NUM_BYTES_RFC4122_UUID);
        domain_connection_token = QUuid::fromRfc4122(token);

        qDebug() << "HifiConnection::ParseHifiResponse() - Domain connection token: " << domain_connection_token;
    }
    else if (response_packet->GetType() == PacketType::DomainList && !domain_connected) {
        qDebug() << "HifiConnection::ParseHifiResponse() - Process domain list";
        QDataStream packet_stream(response_packet->readAll());

        // grab the domain's ID from the beginning of the packet
        QUuid domain_uuid;
        packet_stream >> domain_uuid;

        if (domain_id != domain_uuid) {
            // Recieved packet from different domain.
            qDebug() << "HifiConnection::ParseHifiResponse() - Received packet from different domain";
            return;
        }

        quint16 domain_local_id;
        packet_stream >> domain_local_id;

        // pull our owner (ie. session) UUID from the packet, it's always the first thing
        // The short (16 bit) ID comes next.
        packet_stream >> session_id;
        packet_stream >> local_id;

        // if this was the first domain-server list from this domain, we've now connected
        hifi_response_timer->stop();
        domain_connected = true;

        // pull the permissions/right/privileges for this node out of the stream
        uint new_permissions;
        packet_stream >> new_permissions;
        permissions = (Permissions) new_permissions;

        // Is packet authentication enabled?
        bool is_authenticated;
        packet_stream >> is_authenticated; //TODO: handle authentication of packets

        //qDebug() << permissions << is_authenticated;

        //qDebug() << domain_uuid << domain_local_id << session_id << local_id << new_permissions << is_authenticated;

        // pull each node in the packet
        while (packet_stream.device()->pos() < response_packet->GetDataSize() - response_packet->TotalHeaderSize()) {
            ParseNodeFromPacketStream(packet_stream);
        }
    }
    else if (response_packet->GetType() == PacketType::DomainConnectionDenied) {
        uint8_t reasonCode;
        //uint8_t reasonSize;

        response_packet->read((char *) &reasonCode, sizeof(uint8_t));
        /*response_packet->read((char *) &reasonSize, sizeof(uint8_t));

        QByteArray utfReason;
        utfReason.resize(reasonSize);
        response_packet->read(utfReason.data(), reasonSize);

        QString reason = QString::fromUtf8(utfReason.constData(), reasonSize);*/

        qDebug() << "HifiConnection::ParseHifiResponse() - DomainConnectionDenied - Code: " << reasonCode;  //"Reason: "<< reason;

        if (reasonCode == 2) {
            username = "";
        }
    }
}

void HifiConnection::ParseNodeFromPacketStream(QDataStream& packet_stream)
{
    // setup variables to read into from QDataStream
    NodeType_t node_type;
    QUuid node_uuid, connection_secret_uuid;
    QHostAddress node_public_address, node_local_address;
    quint16 node_public_port, node_local_port;
    uint node_permissions;
    bool is_replicated;
    quint16 session_local_id;

    packet_stream >> node_type >> node_uuid >> node_public_address >> node_public_port >> node_local_address >> node_local_port >> node_permissions
        >> is_replicated >> session_local_id >> connection_secret_uuid;

    // if the public socket address is 0 then it's reachable at the same IP
    // as the domain server
    if (node_public_address.isNull()) {
        node_public_address = public_address;
    }

    //qDebug() << (char) node_type << node_uuid << node_public_address << node_public_port << node_local_address << node_local_port << node_permissions
    //          << is_replicated << session_local_id << connection_secret_uuid;

    Node * node = new Node();
    node->SetNodeID(node_uuid);
    node->SetNodeType(node_type);
    node->SetPublicAddress(node_public_address, node_public_port);
    node->SetLocalAddress(node_local_address, node_local_port);
    node->SetSessionLocalID(session_local_id);
    node->SetDomainSessionLocalID(local_id);
    node->SetIsReplicated(is_replicated);
    node->SetConnectionSecret(connection_secret_uuid);
    node->SetPermissions((Permissions) node_permissions);

    switch (node_type) {
        case NodeType::AssetServer : {
            qDebug() << "HifiConnection::ParseNodeFromPacketStream() - Registering asset server" << node_public_address << node_public_port;
            asset_server = node;
            break;
        }
        case NodeType::AudioMixer : {
            qDebug() << "HifiConnection::ParseNodeFromPacketStream() - Registering audio mixer" << node_public_address << node_public_port;
            audio_mixer = node;
            break;
        }
        case NodeType::AvatarMixer : {
            qDebug() << "HifiConnection::ParseNodeFromPacketStream() - Registering avatar mixer" << node_public_address << node_public_port;
            avatar_mixer = node;
            break;
        }
        case NodeType::MessagesMixer : {
            qDebug() << "HifiConnection::ParseNodeFromPacketStream() - Registering messages mixer" << node_public_address << node_public_port;
            messages_mixer = node;
            break;
        }
        case NodeType::EntityServer : {
            qDebug() << "HifiConnection::ParseNodeFromPacketStream() - Registering entity server" << node_public_address << node_public_port;
            entity_server = node;
            break;
        }
        case NodeType::EntityScriptServer : {
            qDebug() << "HifiConnection::ParseNodeFromPacketStream() - Registering entity script server" << node_public_address << node_public_port;
            entity_script_server = node;
            break;
        }
        default: {
            break;
        }
    }
}

void HifiConnection::SendStunRequest()
{
    if (!finished_domain_id_request || !has_tcp_checked_local_socket) {
        return;
    }

    if (num_requests == HIFI_NUM_INITIAL_REQUESTS_BEFORE_FAIL) {
        qDebug() << "HifiConnection::SendStunRequest() - Stopping stun requests to" << stun_server_hostname << stun_server_port;
        stun_response_timer->stop();
        Q_EMIT Disconnected();
        //disconnect(stun_response_timer, &QTimer::timeout, this, &HifiConnection::SendStunRequest);
        //stun_response_timer->deleteLater();
        return;
    }

    if (!has_completed_current_request) {
        qDebug() << "HifiConnection::SendStunRequest() - Sending initial stun request to" << stun_server_hostname << stun_server_port;
        ++num_requests;
    }
    else {
        //qDebug() << "HifiConnection::SendStunRequest() - Completed stun request";
        return;
    }

    char * stun_request_packet = (char *) malloc(NUM_BYTES_STUN_HEADER);

    int packet_index = 0;
    const uint32_t RFC_5389_MAGIC_COOKIE_NETWORK_ORDER = htonl(RFC_5389_MAGIC_COOKIE);

    // leading zeros + message type
    const uint16_t REQUEST_MESSAGE_TYPE = htons(0x0001);
    memcpy(stun_request_packet + packet_index, &REQUEST_MESSAGE_TYPE, sizeof(REQUEST_MESSAGE_TYPE));
    packet_index += sizeof(REQUEST_MESSAGE_TYPE);

    // message length (no additional attributes are included)
    uint16_t message_length = 0;
    memcpy(stun_request_packet + packet_index, &message_length, sizeof(message_length));
    packet_index += sizeof(message_length);

    memcpy(stun_request_packet + packet_index, &RFC_5389_MAGIC_COOKIE_NETWORK_ORDER, sizeof(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER));
    packet_index += sizeof(RFC_5389_MAGIC_COOKIE_NETWORK_ORDER);

    // transaction ID (random 12-byte unsigned integer)
    const uint NUM_TRANSACTION_ID_BYTES = 12;
    QUuid randomUUID = QUuid::createUuid();
    memcpy(stun_request_packet + packet_index, randomUUID.toRfc4122().data(), NUM_TRANSACTION_ID_BYTES);

    qDebug () << "HifiConnection::SendStunRequest() - STUN address:" << stun_server_address << "STUN port:" << stun_server_port;
    SendServerMessage(stun_request_packet, NUM_BYTES_STUN_HEADER, stun_server_address, stun_server_port);
}

void HifiConnection::SendIceRequest()
{
    if (num_requests == HIFI_NUM_INITIAL_REQUESTS_BEFORE_FAIL) {
        qDebug() << "HifiConnection::SendIceRequest() - Stopping ice requests to" << ice_server_address << ice_server_port;

        ice_response_timer->stop();
        Q_EMIT Disconnected();
        //disconnect(ice_response_timer, &QTimer::timeout, this, &HifiConnection::SendIceRequest);
        //ice_response_timer->deleteLater();
        return;
    }

    if (!has_completed_current_request) {
        qDebug() << "HifiConnection::SendIceRequest() - Sending intial ice request to" << ice_server_address << ice_server_port;

        ++num_requests;
    }
    else {
        //qDebug() << "HifiConnection::SendIceRequest() - Completed ice request";
        //ice_response_timer->stop();
        //disconnect(ice_response_timer, &QTimer::timeout, this, &HifiConnection::SendIceRequest);
        //ice_response_timer->deleteLater();
        return;
    }

    PacketType packetType = PacketType::ICEServerQuery;
    //PacketVersion version = versionForPacketType(packetType);
    std::unique_ptr<Packet> ice_request_packet = Packet::Create(0,packetType);
    QDataStream ice_data_stream(ice_request_packet.get());
    ice_data_stream << ice_client_id << public_address << public_port << local_address << local_port << domain_id;
    //qDebug () << "HifiConnection::SendIceRequest() - ICE address:" << hifi_socket->peerAddress() << "ICE port:" << hifi_socket->peerPort();
    //qDebug() << "ICE packet values" << sequence_number << (uint8_t)packetType << (int)versionForPacketType(packetType) << ice_client_id << public_address << public_port << local_address << local_port << domain_id;

    SendServerMessage(ice_request_packet->GetData(), ice_request_packet->GetDataSize(), ice_server_address, ice_server_port);
}

void HifiConnection::SendDomainCheckIn()
{
    if (!finished_domain_id_request) {
        return;
    }

    if (num_requests == HIFI_NUM_INITIAL_REQUESTS_BEFORE_FAIL) {
            qDebug() << "HifiConnection::SendDomainConnectRequest() - Stopping domain requests to" << domain_place_name;

        hifi_response_timer->stop();
        Q_EMIT Disconnected();
        //hifi_response_timer->deleteLater();
        return;
    }

    if (!domain_connected) {
        qDebug() << "HifiConnection::SendDomainConnectRequest() - Sending initial domain connect request to" << domain_public_address << domain_public_port;
        ++num_requests;
    }
    else {
        //qDebug() << "HifiConnection::SendDomainConnectRequest() - Completed domain connect request";
        //hifi_response_timer->stop();
        //hifi_response_timer->deleteLater();
        return;
    }

    SendDomainCheckInRequest();
}


void HifiConnection::KeypairRequestFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (reply) {
        // double check if the finished network reply had a session ID in the header and make
        // sure that our session ID matches that value if so
        /*if (reply->hasRawHeader(METAVERSE_SESSION_ID_HEADER)) {
            session_id = networkReply->rawHeader(METAVERSE_SESSION_ID_HEADER);
        }*/

        if (reply->error() == QNetworkReply::NoError) {
            qDebug() << "Uploaded public key to Metaverse API. RSA keypair generation is completed.";
            waiting_for_keypair = false;

            //qDebug(networking) << "Received JSON response from metaverse API that has no matching callback.";
            //qDebug(networking) << QJsonDocument::fromJson(networkReply->readAll());
        } else {
            qWarning() << "Public key upload failed from AccountManager" << reply->errorString();

            // we aren't waiting for a response any longer
            waiting_for_keypair = false;
            username = ""; // Reset username so user can still log in

            //qDebug(networking) << "Received error response from metaverse API that has no matching callback.";
            //qDebug(networking) << "Error" << networkReply->error() << "-" << networkReply->errorString();
            //qDebug(networking) << networkReply->readAll();
        }

        reply->close();
        reply->deleteLater();
    }
}

void HifiConnection::SendDomainCheckInRequest(uint32_t s)
{
    if (!finished_domain_id_request) {
        return;
    }

    bool requires_username_signature = !domain_connected && !domain_connection_token.isNull();

    //Generate keypair
    if (requires_username_signature && keypair_generator->GetPrivateKey().isEmpty()) {
        qDebug() << "HifiConnection::SendDomainCheckInRequest() - Generating keypair for username signature";
        num_requests = 0;
        username_signature = QByteArray();
        keypair_generator->GenerateKeypair();

        if (!waiting_for_keypair) {
            waiting_for_keypair = true;

            // upload the public key so data-web has an up-to-date key
            // setup a multipart upload to send up the public key
            QHttpMultiPart* request_multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

            QHttpPart public_key_part;
            public_key_part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/octet-stream"));
            public_key_part.setHeader(QNetworkRequest::ContentDispositionHeader,
                                QVariant("form-data; name=\"public_key\"; filename=\"public_key\""));
            public_key_part.setBody(keypair_generator->GetPublicKey());
            request_multipart->append(public_key_part);

            const QByteArray HIGH_FIDELITY_USER_AGENT = "Mozilla/5.0 (HighFidelityInterface)";
            const auto METAVERSE_SESSION_ID_HEADER = QString("HFM-SessionID").toLocal8Bit();
            const QByteArray ACCESS_TOKEN_AUTHORIZATION_HEADER = "Authorization";

            QNetworkAccessManager * nam = new QNetworkAccessManager(this);
            QNetworkRequest request;
            request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
            request.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);
            request.setRawHeader(METAVERSE_SESSION_ID_HEADER, uuidStringWithoutCurlyBraces(session_id).toLocal8Bit());
            if (token != "") {
                request.setRawHeader(ACCESS_TOKEN_AUTHORIZATION_HEADER, QString("Bearer %1").arg(token).toUtf8());
            }
            request.setUrl(QUrl("https://metaverse.highfidelity.com/api/v1/user/public_key"));

            QNetworkReply* reply = NULL;
            reply = nam->put(request, request_multipart);

            connect(reply, SIGNAL(finished()), this, SLOT(KeypairRequestFinished()));
        }

        return;
    }
    if (waiting_for_keypair) return;

    PacketType packet_type = (domain_connected) ? PacketType::DomainListRequest : PacketType::DomainConnectRequest;
    std::unique_ptr<Packet> domain_checkin_request_packet = Packet::Create(s,packet_type);
    QDataStream domain_checkin_data_stream(domain_checkin_request_packet.get());

    if (packet_type == PacketType::DomainConnectRequest) {
        domain_checkin_data_stream << ice_client_id;

        QByteArray protocol_version_sig = Utils::GetProtocolVersionSignature();
        domain_checkin_data_stream.writeBytes(protocol_version_sig.constData(), protocol_version_sig.size());

        domain_checkin_data_stream << Utils::GetHardwareAddress(local_address) << Utils::GetMachineFingerprint();
        //qDebug() << ice_client_id << protocol_version_sig << Utils::GetHardwareAddress(hifi_socket->localAddress()) << Utils::GetMachineFingerprint() << (char)owner_type.load()
        //         << public_address << public_port << local_address << local_port << node_types_of_interest << domain_place_name;
    }

    //qDebug() << "list request" << (char)owner_type.load() << public_address << public_port << local_address << local_port << node_types_of_interest << domain_place_name;
    domain_checkin_data_stream << owner_type.load() << public_address << public_port <<local_address << local_port
                               << node_types_of_interest.toList() << domain_place_name;

    if (!domain_connected && username != "") {
        domain_checkin_data_stream << username;

        // if this is a connect request, and we can present a username signature, send it along

        if (!keypair_generator->GetPrivateKey().isEmpty()) {
            QByteArray plaintext = username.toLower().toUtf8().append(domain_connection_token.toRfc4122());

            //Sign plaintext
            if (!keypair_generator->GetPrivateKey().isEmpty()) {
                const char* private_key_data = keypair_generator->GetPrivateKey().constData();
                RSA* rsa_private_key = d2i_RSAPrivateKey(NULL,
                                                       reinterpret_cast<const unsigned char**>(&private_key_data),
                                                       keypair_generator->GetPrivateKey().size());
                if (rsa_private_key) {
                    QByteArray signature(RSA_size(rsa_private_key), 0);
                    unsigned int signature_bytes = 0;

                    QByteArray hashed_plaintext = QCryptographicHash::hash(plaintext, QCryptographicHash::Sha256);

                    int encrypt_return = RSA_sign(NID_sha256,
                                                 reinterpret_cast<const unsigned char*>(hashed_plaintext.constData()),
                                                 hashed_plaintext.size(),
                                                 reinterpret_cast<unsigned char*>(signature.data()),
                                                 &signature_bytes,
                                                 rsa_private_key);

                    // free the private key RSA struct now that we are done with it
                    RSA_free(rsa_private_key);

                    if (encrypt_return != -1) {
                        username_signature = signature;
                        //qDebug() << username_signature;
                    }
                    else {
                        username_signature = QByteArray();
                    }
                } else {
                    qDebug() << "Could not create RSA struct from QByteArray private key.";
                }
            }
            else {
                username_signature = QByteArray();
            }

            if (!username_signature.isEmpty()) {
                qDebug() << "Returning username" << username
                    << "signed with connection UUID" << uuidStringWithoutCurlyBraces(domain_connection_token);
            } else {
                qDebug() << "Error signing username with connection token";
                qDebug() << "Will re-attempt on next domain-server check in.";
            }
        }

        if (requires_username_signature && !username_signature.isEmpty()) {
            domain_checkin_data_stream << username_signature;
        }
    }

    if (packet_type == PacketType::DomainListRequest) {
        // Source domain list packets
        domain_checkin_request_packet->WriteSourceID(local_id);
    }

    SendServerMessage(domain_checkin_request_packet->GetData(), domain_checkin_request_packet->GetDataSize(), domain_public_address, domain_public_port);
}

void HifiConnection::SendIcePing(uint32_t s, quint8 ping_type)
{
    int packet_size = NUM_BYTES_RFC4122_UUID + sizeof(quint8);

    auto ice_ping = Packet::Create(s, PacketType::ICEPing, packet_size);
    ice_ping->write(ice_client_id.toRfc4122());
    ice_ping->write(reinterpret_cast<const char*>(&ping_type), sizeof(ping_type));

    //QByteArray b(ice_ping_packet->GetData(), ice_ping_packet->GetDataSize());
    //qDebug() << "ping packet" << b;
    SendServerMessage(ice_ping->GetData(), ice_ping->GetDataSize(), (ping_type == 1)?domain_local_address:domain_public_address, (ping_type == 1)?domain_local_port:domain_public_port);
}

void HifiConnection::SendIcePingReply(uint32_t s, quint8 ping_type)
{
    int packet_size = NUM_BYTES_RFC4122_UUID + sizeof(quint8);
    std::unique_ptr<Packet> ice_ping_reply = Packet::Create(s, PacketType::ICEPingReply, packet_size);

    // pack the ICE ID and then the ping type
    ice_ping_reply->write(ice_client_id.toRfc4122());
    ice_ping_reply->write(reinterpret_cast<const char*>(&ping_type), sizeof(ping_type));

    //QByteArray b(ice_ping_reply->GetData(), ice_ping_reply->GetDataSize());
    //qDebug() << "ping reply packet" << b;

    //qDebug() << packet_size << ice_ping_reply->GetDataSize();
    SendServerMessage(ice_ping_reply->GetData(), ice_ping_reply->GetDataSize(), (ping_type == 1)?domain_local_address:domain_public_address, (ping_type == 1)?domain_local_port:domain_public_port);
}

void HifiConnection::ClientMessageReceived(const QString &message)
{
    //qDebug() << "HifiConnection::ClientMessageReceived() - " << message;
    if (started_hifi_connect) return;

    QJsonDocument doc;
    doc = QJsonDocument::fromJson(message.toLatin1());
    QJsonObject obj = doc.object();

    if (!obj.keys().contains("type")) return;

    QString type = obj["type"].toString();

    if (type == "domain") {
        // Domain ID lookup
        if (domain_name == "") {
            domain_name = obj["domain_name"].toString();

            if (domain_name.left(7) == "hifi://") {
                domain_name = domain_name.remove("hifi://");
            }

            qDebug() << "HifiConnection::ClientMessageReceived - Looking up domain ID for domain: " << domain_name;

            QNetworkAccessManager * nam = new QNetworkAccessManager(this);
            QNetworkRequest id_request("https://metaverse.highfidelity.com/api/v1/places/" + domain_name);
            id_request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            QNetworkReply * reply = nam->get(id_request);
            connect(reply, SIGNAL(finished()), this, SLOT(DomainRequestFinished()));
        }

        if (username == "") {
            if (obj.keys().contains("username") && obj.keys().contains("password")) {
                username = obj["username"].toString();
                password = obj["password"].toString();

                QNetworkAccessManager * nam = new QNetworkAccessManager(this);
                QNetworkRequest user_request;
                const QByteArray HIGH_FIDELITY_USER_AGENT = "Mozilla/5.0 (HighFidelityInterface)";
                const QString ACCOUNT_MANAGER_REQUESTED_SCOPE = "owner";
                user_request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
                user_request.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);

                QByteArray post_data;
                post_data.append("grant_type=password&");
                post_data.append("username=" + username + "&");
                post_data.append("password=" + QUrl::toPercentEncoding(password) + "&");
                post_data.append("scope=" + ACCOUNT_MANAGER_REQUESTED_SCOPE);

                user_request.setUrl(QUrl("https://metaverse.highfidelity.com/oauth/token"));
                user_request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

                QNetworkReply* reply = nam->post(user_request, post_data);
                connect(reply, &QNetworkReply::finished, this, &HifiConnection::RequestAccessTokenFinished);
                connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(RequestAccessTokenError(QNetworkReply::NetworkError)));
            }
        }
    }
    else if (type == "offer") {
        std::function<void(rtcdcpp::PeerConnection::IceCandidate)> onLocalIceCandidate = [this](rtcdcpp::PeerConnection::IceCandidate candidate) {
            if (QString::fromStdString(candidate.candidate) != "") {
                QJsonObject candidate_object;
                candidate_object.insert("type", QJsonValue::fromVariant("candidate"));
                QJsonObject candidate_object2;
                candidate_object2.insert("candidate", QJsonValue::fromVariant(QString::fromStdString(candidate.candidate)));
                candidate_object2.insert("sdpMid", QJsonValue::fromVariant(QString::fromStdString(candidate.sdpMid)));
                candidate_object2.insert("sdpMLineIndex", QJsonValue::fromVariant(candidate.sdpMLineIndex));
                candidate_object.insert("candidate", candidate_object2);
                QJsonDocument candidateDoc(candidate_object);

                //qDebug() << "candidate: " << candidateDoc.toJson();
                if (this->client_socket) this->client_socket->sendTextMessage(QString::fromStdString(candidateDoc.toJson(QJsonDocument::Compact).toStdString()));
            }
        };

        std::function<void(std::shared_ptr<rtcdcpp::DataChannel> channel)> onDataChannel = [this](std::shared_ptr<rtcdcpp::DataChannel> channel) {
            //qDebug() << "datachannel" << QString::fromStdString(channel->GetLabel());
            QString label = QString::fromStdString(channel->GetLabel());
            if (label == "datachannel") {
                qDebug() << "HifiConnection::onDataChannel() - Registering domain server data channel";
                data_channel = channel;
            }

            if (this->data_channel && this->finished_domain_id_request && !this->started_hifi_connect) {
                qDebug() << "HifiConnection::WebRTCConnectionReady() - Data channels registered";
                started_hifi_connect = true;
                Q_EMIT WebRTCConnectionReady();
            }
        };

        rtcdcpp::RTCConfiguration config;
        config.ice_servers.emplace_back(rtcdcpp::RTCIceServer{"stun.l.google.com", 19302});

        remote_peer_connection = std::make_shared<rtcdcpp::PeerConnection>(config, onLocalIceCandidate, onDataChannel);

        remote_peer_connection->ParseOffer(obj["sdp"].toString().toStdString());
        QJsonObject answer_object;
        answer_object.insert("type", QJsonValue::fromVariant("answer"));
        answer_object.insert("sdp", QJsonValue::fromVariant(QString::fromStdString(remote_peer_connection->GenerateAnswer())));
        QJsonDocument answerDoc(answer_object);

        //qDebug() << "Sending Answer: " << answerDoc.toJson();
        if (this->client_socket) client_socket->sendTextMessage(QString::fromStdString(answerDoc.toJson(QJsonDocument::Compact).toStdString()));
    }
    else if (type == "candidate") {
        //qDebug() << "remote candidate";
        QJsonObject c = obj["candidate"].toObject();
        remote_peer_connection->SetRemoteIceCandidate("a=" + c["candidate"].toString().toStdString());
    }
}

void HifiConnection::ServerDisconnected()
{
    Q_EMIT Disconnected();
}

Node * HifiConnection::GetNodeFromAddress(QHostAddress sender, quint16 sender_port)
{
    Node * node = nullptr;
    if (audio_mixer && audio_mixer->CheckNodeAddress(sender, sender_port))
        node = audio_mixer;
    else if (avatar_mixer && avatar_mixer->CheckNodeAddress(sender, sender_port))
        node = avatar_mixer;
    else if (asset_server && asset_server->CheckNodeAddress(sender, sender_port))
        node = asset_server;
    else if (messages_mixer && messages_mixer->CheckNodeAddress(sender, sender_port))
        node = messages_mixer;
    else if (entity_script_server && entity_script_server->CheckNodeAddress(sender, sender_port))
        node = entity_script_server;
    else if (entity_server && entity_server->CheckNodeAddress(sender, sender_port))
        node = entity_server;

    return node;
}
