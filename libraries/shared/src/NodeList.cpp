//
//  NodeList.cpp
//  hifi
//
//  Created by Stephen Birarda on 2/15/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <QDebug>

#include "NodeList.h"
#include "NodeTypes.h"
#include "PacketHeaders.h"
#include "SharedUtil.h"

#ifdef _WIN32
#include "Syssocket.h"
#else
#include <arpa/inet.h>
#endif

const char SOLO_NODE_TYPES[3] = {
    NODE_TYPE_AVATAR_MIXER,
    NODE_TYPE_AUDIO_MIXER,
    NODE_TYPE_VOXEL_SERVER
};

const char DEFAULT_DOMAIN_HOSTNAME[MAX_HOSTNAME_BYTES] = "root.highfidelity.io";
const char DEFAULT_DOMAIN_IP[INET_ADDRSTRLEN] = "";    //  IP Address will be re-set by lookup on startup
const int DEFAULT_DOMAINSERVER_PORT = 40102;

bool silentNodeThreadStopFlag = false;
bool pingUnknownNodeThreadStopFlag = false;

NodeList* NodeList::_sharedInstance = NULL;

NodeList* NodeList::createInstance(char ownerType, unsigned int socketListenPort) {
    if (!_sharedInstance) {
        _sharedInstance = new NodeList(ownerType, socketListenPort);
    } else {
        qDebug("NodeList createInstance called with existing instance.\n");
    }
    
    return _sharedInstance;
}

NodeList* NodeList::getInstance() {
    if (!_sharedInstance) {
        qDebug("NodeList getInstance called before call to createInstance. Returning NULL pointer.\n");
    }
    
    return _sharedInstance;
}

NodeList::NodeList(char newOwnerType, unsigned int newSocketListenPort) :
    _nodeBuckets(),
    _numNodes(0),
    _nodeSocket(newSocketListenPort),
    _ownerType(newOwnerType),
    _nodeTypesOfInterest(NULL),
    _ownerID(UNKNOWN_NODE_ID),
    _lastNodeID(0)
{
    memcpy(_domainHostname, DEFAULT_DOMAIN_HOSTNAME, sizeof(DEFAULT_DOMAIN_HOSTNAME));
    memcpy(_domainIP, DEFAULT_DOMAIN_IP, sizeof(DEFAULT_DOMAIN_IP));
}

NodeList::~NodeList() {
    delete _nodeTypesOfInterest;
    
    clear();
    
    // stop the spawned threads, if they were started
    stopSilentNodeRemovalThread();
}

void NodeList::setDomainHostname(const char* domainHostname) {    
    memset(_domainHostname, 0, sizeof(_domainHostname));
    memcpy(_domainHostname, domainHostname, strlen(domainHostname));
    
    // reset the domain IP so the hostname is checked again
    setDomainIP("");
}

void NodeList::setDomainIP(const char* domainIP) {
    memset(_domainIP, 0, sizeof(_domainIP));
    memcpy(_domainIP, domainIP, strlen(domainIP));
}

void NodeList::setDomainIPToLocalhost() {
    int ip = getLocalAddress();
    sprintf(_domainIP, "%d.%d.%d.%d", (ip & 0xFF), ((ip >> 8) & 0xFF),((ip >> 16) & 0xFF), ((ip >> 24) & 0xFF));
}

void NodeList::timePingReply(sockaddr *nodeAddress, unsigned char *packetData) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        if (socketMatch(node->getPublicSocket(), nodeAddress) || 
            socketMatch(node->getLocalSocket(), nodeAddress)) {     

            int pingTime = usecTimestampNow() - *(uint64_t*)(packetData + numBytesForPacketHeader(packetData));
            
            node->setPingMs(pingTime / 1000);
            break;
        }
    }
}

void NodeList::processNodeData(sockaddr* senderAddress, unsigned char* packetData, size_t dataBytes) {
    switch (packetData[0]) {
        case PACKET_TYPE_DOMAIN: {
            // only process the DS if this is our current domain server
            sockaddr_in domainServerSocket = *(sockaddr_in*) senderAddress;
            const char* domainSenderIP = inet_ntoa(domainServerSocket.sin_addr);
            
            if (memcmp(domainSenderIP, _domainIP, strlen(domainSenderIP)) == 0) {
                processDomainServerList(packetData, dataBytes);
            }
            
            break;
        }
        case PACKET_TYPE_PING: {
            char *pingPacket;
			pingPacket = new char[dataBytes];
            memcpy(pingPacket, packetData, dataBytes);
            populateTypeAndVersion((unsigned char*) pingPacket, PACKET_TYPE_PING_REPLY);
            _nodeSocket.send(senderAddress, pingPacket, dataBytes);
			delete[] pingPacket;
            break;
        }
        case PACKET_TYPE_PING_REPLY: {
            timePingReply(senderAddress, packetData);
            break;
        }
    }
}

void NodeList::processBulkNodeData(sockaddr *senderAddress, unsigned char *packetData, int numTotalBytes) {
    
    // find the avatar mixer in our node list and update the lastRecvTime from it
    Node* bulkSendNode = nodeWithAddress(senderAddress);

    if (bulkSendNode) {
        bulkSendNode->setLastHeardMicrostamp(usecTimestampNow());
        bulkSendNode->recordBytesReceived(numTotalBytes);
        
        int numBytesPacketHeader = numBytesForPacketHeader(packetData);
        
        unsigned char* startPosition = packetData;
        unsigned char* currentPosition = startPosition + numBytesPacketHeader;
        unsigned char *packetHolder;
		packetHolder = new unsigned char[numTotalBytes];
        
        // we've already verified packet version for the bulk packet, so all head data in the packet is also up to date
        populateTypeAndVersion(packetHolder, PACKET_TYPE_HEAD_DATA);
        
        uint16_t nodeID = -1;
        
        while ((currentPosition - startPosition) < numTotalBytes) {
            unpackNodeId(currentPosition, &nodeID);
            memcpy(packetHolder + numBytesPacketHeader,
                   currentPosition,
                   numTotalBytes - (currentPosition - startPosition));
            
            Node* matchingNode = nodeWithID(nodeID);
            
            if (!matchingNode) {
                // we're missing this node, we need to add it to the list
                matchingNode = addOrUpdateNode(NULL, NULL, NODE_TYPE_AGENT, nodeID);
            }
            
            currentPosition += updateNodeWithData(matchingNode,
                                                  packetHolder,
                                                  numTotalBytes - (currentPosition - startPosition));
			delete[] packetHolder;
            
        }
    }    
}

int NodeList::updateNodeWithData(sockaddr *senderAddress, unsigned char *packetData, size_t dataBytes) {
    // find the node by the sockaddr
    Node* matchingNode = nodeWithAddress(senderAddress);
    
    if (matchingNode) {
        return updateNodeWithData(matchingNode, packetData, dataBytes);
    } else {
        return 0;
    }
}

int NodeList::updateNodeWithData(Node *node, unsigned char *packetData, int dataBytes) {
    node->lock();
    
    node->setLastHeardMicrostamp(usecTimestampNow());
    
    if (node->getActiveSocket()) {
        node->recordBytesReceived(dataBytes);
    }
    
    if (!node->getLinkedData() && linkedDataCreateCallback) {
        linkedDataCreateCallback(node);
    }
    
    int numParsedBytes = node->getLinkedData()->parseData(packetData, dataBytes);
    
    node->unlock();
    
    return numParsedBytes;
}

Node* NodeList::nodeWithAddress(sockaddr *senderAddress) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        if (node->getActiveSocket() && socketMatch(node->getActiveSocket(), senderAddress)) {
            return &(*node);
        }
    }
    
    return NULL;
}

Node* NodeList::nodeWithID(uint16_t nodeID) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        if (node->getNodeID() == nodeID) {
            return &(*node);
        }
    }

    return NULL;
}

int NodeList::getNumAliveNodes() const {
    int numAliveNodes = 0;
    
    for (NodeList::iterator node = begin(); node != end(); node++) {
        if (node->isAlive()) {
            ++numAliveNodes;
        }
    }
    
    return numAliveNodes;
}

void NodeList::clear() {
    // delete all of the nodes in the list, set the pointers back to NULL and the number of nodes to 0
    for (int i = 0; i < _numNodes; i++) {
        Node** nodeBucket = _nodeBuckets[i / NODES_PER_BUCKET];
        Node* node = nodeBucket[i % NODES_PER_BUCKET];
        
        node->lock();
        delete node;
        
        node = NULL;
    }
    
    _numNodes = 0;
}

void NodeList::setNodeTypesOfInterest(const char* nodeTypesOfInterest, int numNodeTypesOfInterest) {
    delete _nodeTypesOfInterest;
    
    _nodeTypesOfInterest = new char[numNodeTypesOfInterest + sizeof(char)];
    memcpy(_nodeTypesOfInterest, nodeTypesOfInterest, numNodeTypesOfInterest);
    _nodeTypesOfInterest[numNodeTypesOfInterest] = '\0';
}

void NodeList::sendDomainServerCheckIn() {
    static bool printedDomainServerIP = false;
    
    //  Lookup the IP address of the domain server if we need to
    if (atoi(_domainIP) == 0) {
        printf("Looking up %s\n", _domainHostname);
        struct hostent* pHostInfo;
        if ((pHostInfo = gethostbyname(_domainHostname)) != NULL) {
            sockaddr_in tempAddress;
            memcpy(&tempAddress.sin_addr, pHostInfo->h_addr_list[0], pHostInfo->h_length);
            strcpy(_domainIP, inet_ntoa(tempAddress.sin_addr));

            qDebug("Domain Server: %s\n", _domainHostname);
        } else {
            qDebug("Failed domain server lookup\n");
        }
    } else if (!printedDomainServerIP) {
        qDebug("Domain Server IP: %s\n", _domainIP);
        printedDomainServerIP = true;
    }
    
    static unsigned char* checkInPacket = NULL;
    static int checkInPacketSize = 0;
    
    // construct the DS check in packet if we need to    
    if (!checkInPacket) {
        int numBytesNodesOfInterest = _nodeTypesOfInterest ? strlen((char*) _nodeTypesOfInterest) : 0;
        
        const int IP_ADDRESS_BYTES = 4;
        
        // check in packet has header, node type, port, IP, node types of interest, null termination
        int numPacketBytes = sizeof(PACKET_TYPE) + sizeof(PACKET_VERSION) + sizeof(NODE_TYPE) + sizeof(uint16_t) +
            IP_ADDRESS_BYTES + numBytesNodesOfInterest + sizeof(unsigned char);
        
        checkInPacket = new unsigned char[numPacketBytes];
        unsigned char* packetPosition = checkInPacket;
        
        PACKET_TYPE nodePacketType = (memchr(SOLO_NODE_TYPES, _ownerType, sizeof(SOLO_NODE_TYPES)))
            ? PACKET_TYPE_DOMAIN_REPORT_FOR_DUTY
            : PACKET_TYPE_DOMAIN_LIST_REQUEST;
        
        int numHeaderBytes = populateTypeAndVersion(packetPosition, nodePacketType);
        packetPosition += numHeaderBytes;
        
        *(packetPosition++) = _ownerType;
        
        packetPosition += packSocket(checkInPacket + numHeaderBytes + sizeof(NODE_TYPE),
                                     getLocalAddress(),
                                     htons(_nodeSocket.getListeningPort()));
        
        // add the number of bytes for node types of interest
        *(packetPosition++) = numBytesNodesOfInterest;
                
        // copy over the bytes for node types of interest, if required
        if (numBytesNodesOfInterest > 0) {
            memcpy(packetPosition,
                   _nodeTypesOfInterest,
                   numBytesNodesOfInterest);
            packetPosition += numBytesNodesOfInterest;
        }
        
        checkInPacketSize = packetPosition - checkInPacket;
    }
    
    _nodeSocket.send(_domainIP, DEFAULT_DOMAINSERVER_PORT, checkInPacket, checkInPacketSize);
}

int NodeList::processDomainServerList(unsigned char* packetData, size_t dataBytes) {
    int readNodes = 0;

    char nodeType;
    uint16_t nodeId;
    
    // assumes only IPv4 addresses
    sockaddr_in nodePublicSocket;
    nodePublicSocket.sin_family = AF_INET;
    sockaddr_in nodeLocalSocket;
    nodeLocalSocket.sin_family = AF_INET;
    
    unsigned char* readPtr = packetData + numBytesForPacketHeader(packetData);
    unsigned char* startPtr = packetData;
    
    while((readPtr - startPtr) < dataBytes - sizeof(uint16_t)) {
        nodeType = *readPtr++;
        readPtr += unpackNodeId(readPtr, (uint16_t*) &nodeId);
        readPtr += unpackSocket(readPtr, (sockaddr*) &nodePublicSocket);
        readPtr += unpackSocket(readPtr, (sockaddr*) &nodeLocalSocket);
        
        // if the public socket address is 0 then it's reachable at the same IP
        // as the domain server
        if (nodePublicSocket.sin_addr.s_addr == 0) {
            inet_aton(_domainIP, &nodePublicSocket.sin_addr);
        }
        
        addOrUpdateNode((sockaddr*) &nodePublicSocket, (sockaddr*) &nodeLocalSocket, nodeType, nodeId);
    }
    
    // read out our ID from the packet
    unpackNodeId(readPtr, &_ownerID);

    return readNodes;
}

Node* NodeList::addOrUpdateNode(sockaddr* publicSocket, sockaddr* localSocket, char nodeType, uint16_t nodeId) {
    NodeList::iterator node = end();
    
    if (publicSocket) {
        for (node = begin(); node != end(); node++) {
            if (node->matches(publicSocket, localSocket, nodeType)) {
                // we already have this node, stop checking
                break;
            }
        }
    } 
    
    if (node == end()) {
        // we didn't have this node, so add them
        Node* newNode = new Node(publicSocket, localSocket, nodeType, nodeId);
        
        if (socketMatch(publicSocket, localSocket)) {
            // likely debugging scenario with two nodes on local network
            // set the node active right away
            newNode->activatePublicSocket();
        }
   
        if (newNode->getType() == NODE_TYPE_VOXEL_SERVER ||
            newNode->getType() == NODE_TYPE_AVATAR_MIXER ||
            newNode->getType() == NODE_TYPE_AUDIO_MIXER) {
            // this is currently the cheat we use to talk directly to our test servers on EC2
            // to be removed when we have a proper identification strategy
            newNode->activatePublicSocket();
        }
        
        addNodeToList(newNode);
        
        return newNode;
    } else {
        
        if (node->getType() == NODE_TYPE_AUDIO_MIXER ||
            node->getType() == NODE_TYPE_VOXEL_SERVER) {
            // until the Audio class also uses our nodeList, we need to update
            // the lastRecvTimeUsecs for the audio mixer so it doesn't get killed and re-added continously
            node->setLastHeardMicrostamp(usecTimestampNow());
        }
        
        // we had this node already, do nothing for now
        return &*node;
    }    
}

void NodeList::addNodeToList(Node* newNode) {
    // find the correct array to add this node to
    int bucketIndex = _numNodes / NODES_PER_BUCKET;
    
    if (!_nodeBuckets[bucketIndex]) {
        _nodeBuckets[bucketIndex] = new Node*[NODES_PER_BUCKET]();
    }
    
    _nodeBuckets[bucketIndex][_numNodes % NODES_PER_BUCKET] = newNode;
    
    ++_numNodes;
    
    qDebug() << "Added" << *newNode << "\n";
}

unsigned NodeList::broadcastToNodes(unsigned char *broadcastData, size_t dataBytes, const char* nodeTypes, int numNodeTypes) {
    unsigned n = 0;
    for(NodeList::iterator node = begin(); node != end(); node++) {
        // only send to the NodeTypes we are asked to send to.
        if (node->getActiveSocket() != NULL && memchr(nodeTypes, node->getType(), numNodeTypes)) {
            // we know which socket is good for this node, send there
            _nodeSocket.send(node->getActiveSocket(), broadcastData, dataBytes);
            ++n;
        }
    }
    return n;
}

void NodeList::handlePingReply(sockaddr *nodeAddress) {
    for(NodeList::iterator node = begin(); node != end(); node++) {
        // check both the public and local addresses for each node to see if we find a match
        // prioritize the private address so that we prune erroneous local matches
        if (socketMatch(node->getPublicSocket(), nodeAddress)) {
            node->activatePublicSocket();
            break;
        } else if (socketMatch(node->getLocalSocket(), nodeAddress)) {
            node->activateLocalSocket();
            break;
        }
    }
}

Node* NodeList::soloNodeOfType(char nodeType) {
    if (memchr(SOLO_NODE_TYPES, nodeType, sizeof(SOLO_NODE_TYPES)) != NULL) {
        for(NodeList::iterator node = begin(); node != end(); node++) {
            if (node->getType() == nodeType) {
                return &(*node);
            }
        }
    }
    
    return NULL;
}

void *removeSilentNodes(void *args) {
    NodeList* nodeList = (NodeList*) args;
    uint64_t checkTimeUSecs;
    int sleepTime;
    
    while (!silentNodeThreadStopFlag) {
        checkTimeUSecs = usecTimestampNow();
        
        for(NodeList::iterator node = nodeList->begin(); node != nodeList->end(); ++node) {
            
            if ((checkTimeUSecs - node->getLastHeardMicrostamp()) > NODE_SILENCE_THRESHOLD_USECS) {
            
                qDebug() << "Killed" << *node << "\n";
                
                node->setAlive(false);
            }
        }
        
        sleepTime = NODE_SILENCE_THRESHOLD_USECS - (usecTimestampNow() - checkTimeUSecs);
        #ifdef _WIN32
        Sleep( static_cast<int>(1000.0f*sleepTime) );
        #else
        usleep(sleepTime);
        #endif
    }
    
    pthread_exit(0);
    return NULL;
}

void NodeList::startSilentNodeRemovalThread() {
    pthread_create(&removeSilentNodesThread, NULL, removeSilentNodes, (void*) this);
}

void NodeList::stopSilentNodeRemovalThread() {
    silentNodeThreadStopFlag = true;
    pthread_join(removeSilentNodesThread, NULL);
    
}

const QString QSETTINGS_GROUP_NAME = "NodeList";
const QString DOMAIN_SERVER_SETTING_KEY = "domainServerHostname";

void NodeList::loadData(QSettings *settings) {
    settings->beginGroup(DOMAIN_SERVER_SETTING_KEY);
    
    QString domainServerHostname = settings->value(DOMAIN_SERVER_SETTING_KEY).toString();
    
    if (domainServerHostname.size() > 0) {
        memset(_domainHostname, 0, MAX_HOSTNAME_BYTES);
        memcpy(_domainHostname, domainServerHostname.toLocal8Bit().constData(), domainServerHostname.size());
    }
    
    settings->endGroup();
}

void NodeList::saveData(QSettings* settings) {
    settings->beginGroup(DOMAIN_SERVER_SETTING_KEY);
    
    if (memcmp(_domainHostname, DEFAULT_DOMAIN_HOSTNAME, strlen(DEFAULT_DOMAIN_HOSTNAME)) != 0) {
        // the user is using a different hostname, store it
        settings->setValue(DOMAIN_SERVER_SETTING_KEY, QVariant(_domainHostname));
    } else {
        // the user has switched back to default, remove the current setting
        settings->remove(DOMAIN_SERVER_SETTING_KEY);
    }
    
    settings->endGroup();
}

NodeList::iterator NodeList::begin() const {
    Node** nodeBucket = NULL;
    
    for (int i = 0; i < _numNodes; i++) {
        if (i % NODES_PER_BUCKET == 0) {
            nodeBucket =  _nodeBuckets[i / NODES_PER_BUCKET];
        }
        
        if (nodeBucket[i % NODES_PER_BUCKET]->isAlive()) {
            return NodeListIterator(this, i);
        }
    }
    
    // there's no alive node to start from - return the end
    return end();
}

NodeList::iterator NodeList::end() const {
    return NodeListIterator(this, _numNodes);
}

NodeListIterator::NodeListIterator(const NodeList* nodeList, int nodeIndex) :
    _nodeIndex(nodeIndex) {
    _nodeList = nodeList;
}

NodeListIterator& NodeListIterator::operator=(const NodeListIterator& otherValue) {
    _nodeList = otherValue._nodeList;
    _nodeIndex = otherValue._nodeIndex;
    return *this;
}

bool NodeListIterator::operator==(const NodeListIterator &otherValue) {
    return _nodeIndex == otherValue._nodeIndex;
}

bool NodeListIterator::operator!=(const NodeListIterator &otherValue) {
    return !(*this == otherValue);
}

Node& NodeListIterator::operator*() {
    Node** nodeBucket = _nodeList->_nodeBuckets[_nodeIndex / NODES_PER_BUCKET];
    return *nodeBucket[_nodeIndex % NODES_PER_BUCKET];
}

Node* NodeListIterator::operator->() {
    Node** nodeBucket = _nodeList->_nodeBuckets[_nodeIndex / NODES_PER_BUCKET];
    return nodeBucket[_nodeIndex % NODES_PER_BUCKET];
}

NodeListIterator& NodeListIterator::operator++() {
    skipDeadAndStopIncrement();
    return *this;
}

NodeList::iterator NodeListIterator::operator++(int) {
    NodeListIterator newIterator = NodeListIterator(*this);
    skipDeadAndStopIncrement();
    return newIterator;
}

void NodeListIterator::skipDeadAndStopIncrement() {
    while (_nodeIndex != _nodeList->_numNodes) {
        ++_nodeIndex;
        
        if (_nodeIndex == _nodeList->_numNodes) {
            break;
        } else if ((*(*this)).isAlive()) {
            // skip over the dead nodes
            break;
        }
    }
}
