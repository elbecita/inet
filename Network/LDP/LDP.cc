//
// (C) 2004 Andras Varga
//
// This library is free software, you can redistribute it
// and/or modify
// it under  the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation;
// either version 2 of the License, or any later version.
// The library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//

#include <omnetpp.h>
#include <iostream>
#include <fstream>
#include "ConstType.h"
#include "LDP.h"
#include "LIBtable.h"
#include "MPLSModule.h"
#include "InterfaceTableAccess.h"
#include "IPv4InterfaceData.h"
#include "RoutingTableAccess.h"
#include "UDPControlInfo_m.h"
#include "UDPPacket.h"
#include "TCPSegment.h"
#include "stlwatch.h"

#include "NotifierConsts.h"

#include <algorithm>

Define_Module(LDP);


std::ostream& operator<<(std::ostream& os, const LDP::fec_bind_t& f)
{
    os << "fecid=" << f.fecid << "  peer=" << f.peer << " label=" << f.label;
    return os;
}

bool fecPrefixCompare(const LDP::fec_t& a, const LDP::fec_t& b)
{
	return a.length > b.length;
}

std::ostream& operator<<(std::ostream& os, const LDP::fec_t& f)
{
    os << "fecid=" << f.fecid << "  addr=" << f.addr << "  length=" << f.length << "  nextHop=" << f.nextHop;
    return os;
}

std::ostream& operator<<(std::ostream& os, const LDP::pending_req_t& r)
{
	os << "fecid=" << r.fecid << "  peer=" << r.peer;
	return os;
}

std::ostream& operator<<(std::ostream& os, const LDP::peer_info& p)
{
    os << "peerIP=" << p.peerIP << "  interface=" << p.linkInterface <<
          "  activeRole=" << (p.activeRole ? "true" : "false") <<
          "  socket=" << (p.socket ? TCPSocket::stateName(p.socket->state()) : "NULL");
    return os;
}

bool operator==(const FEC_TLV& a, const FEC_TLV& b)
{
	return a.length == b.length && a.addr == b.addr;
}

bool operator!=(const FEC_TLV& a, const FEC_TLV& b)
{
	return !operator==(a, b);
}

std::ostream& operator<<(std::ostream& os, const FEC_TLV& a)
{
    os << "addr=" << a.addr << "  length=" << a.length;
    return os;
}

void LDP::initialize(int stage)
{
	if(stage != 3)
		return; // wait for routing table to initialize first
		
    holdTime = par("holdTime").doubleValue();
    helloInterval = par("helloInterval").doubleValue();

    ift = InterfaceTableAccess().get();
    rt = RoutingTableAccess().get();
    lt = libTableAccess.get();
    tedmod = tedAccess.get();

    WATCH_VECTOR(myPeers);
    WATCH_VECTOR(fecUp);
	WATCH_VECTOR(fecDown);
    WATCH_VECTOR(fecList);
    WATCH_VECTOR(pending);
    
    maxFecid = 0;

    // schedule first hello
    sendHelloMsg = new cMessage("LDPSendHello");
    scheduleAt(simTime() + exponential(0.1), sendHelloMsg); 

    // start listening for incoming conns
    ev << "Starting to listen on port " << LDP_PORT << " for incoming LDP sessions\n";
    serverSocket.setOutputGate(gate("to_tcp_interface"));
    serverSocket.bind(LDP_PORT);
    serverSocket.listen();

    // build list of recognized FECs
    rebuildFecList();
    
    // listen for routing table modifications
    NotificationBoard *nb = check_and_cast<NotificationBoard*>(parentModule()->submodule("notificationBoard"));
	nb->subscribe(this, NF_IPv4_ROUTINGTABLE_CHANGED);
}

void LDP::handleMessage(cMessage *msg)
{
    if (msg==sendHelloMsg)
    {
        // every LDP capable router periodically sends HELLO messages to the
        // "all routers in the sub-network" multicast address
        ev << "Multicasting LDP Hello to neighboring routers\n";
        sendHelloTo(IPAddress::ALL_ROUTERS_MCAST);

        // schedule next hello
        scheduleAt(simTime() + helloInterval, sendHelloMsg);
    }
    else if (msg->isSelfMessage())
    {
    	if(!strcmp(msg->name(), "HelloTimeout"))
    	{
	    	processHelloTimeout(msg);
    	}
    	else
    	{
    		processNOTIFICATION(check_and_cast<LDPNotify*>(msg));
    	}
    }
    else if (!strcmp(msg->arrivalGate()->name(), "from_udp_interface"))
    {
        // we can only receive LDP Hello from UDP (everything else goes over TCP)
        processLDPHello(check_and_cast<LDPHello *>(msg));
    }
    else if (!strcmp(msg->arrivalGate()->name(), "from_tcp_interface"))
    {
        processMessageFromTCP(msg);
    }
}

void LDP::sendToPeer(IPAddress dest, cMessage *msg)
{
	peerSocket(dest)->send(msg);
}

void LDP::sendMappingRequest(IPAddress dest, IPAddress addr, int length)
{
    LDPLabelRequest *requestMsg = new LDPLabelRequest("Lb-Req");
    requestMsg->setLength(LDP_HEADER_BYTES);
    requestMsg->setType(LABEL_REQUEST);

	FEC_TLV fec;
	fec.addr = addr;
	fec.length = length;
    requestMsg->setFec(fec);

    requestMsg->setReceiverAddress(dest);
    requestMsg->setSenderAddress(rt->getRouterId());
    requestMsg->setLength(30*8); // FIXME find out actual length
    
    sendToPeer(dest, requestMsg);
}

void LDP::updateFecListEntry(LDP::fec_t oldItem)
{
	// do we have mapping from downstream?
	FecBindVector::iterator dit = findFecEntry(fecDown, oldItem.fecid, oldItem.nextHop);
			
	// is next hop our LDP peer?
	bool ER = !peerSocketSoft(oldItem.nextHop);

	ASSERT(!(ER && dit != fecDown.end())); // can't be egress and have mapping at the same time

	// adjust upstream mappings			
	FecBindVector::iterator uit;
	for(uit = fecUp.begin(); uit != fecUp.end(); uit++)
	{
		if(uit->fecid != oldItem.fecid)
			continue;
			
		std::string inInterface = findInterfaceFromPeerAddr(uit->peer);
		std::string outInterface = findInterfaceFromPeerAddr(oldItem.nextHop);
		if(ER)
		{
			// we are egress, that's easy:
			LabelOpVector outLabel = LIBTable::popLabel();
			uit->label = lt->installLibEntry(uit->label, inInterface, outLabel, outInterface, LDP_USER_TRAFFIC);
				
			ev << "installed (egress) LIB entry inLabel=" << uit->label << " inInterface=" << inInterface <<
					" outLabel=" << outLabel << " outInterface=" << outInterface << endl;
		}
		else if(dit != fecDown.end())
		{
			// we have mapping from DS, that's easy
			LabelOpVector outLabel = LIBTable::swapLabel(dit->label);
			uit->label = lt->installLibEntry(uit->label, inInterface, outLabel, outInterface, LDP_USER_TRAFFIC);
			
			ev << "installed LIB entry inLabel=" << uit->label << " inInterface=" << inInterface <<
					" outLabel=" << outLabel << " outInterface=" << outInterface << endl;
		
		}
		else
		{
			// no mapping from DS, withdraw mapping US
			
			ev << "sending withdraw message upstream" << endl;
				
			sendMapping(LABEL_WITHDRAW, uit->peer, uit->label, oldItem.addr, oldItem.length);
			
			// remove from US mappings
			
			fecUp.erase(uit--);
		}
	}
	
	if(!ER && dit == fecDown.end())
	{
		// and ask DS for mapping
		
		ev << "sending request message downstream" << endl;
		
		sendMappingRequest(oldItem.nextHop, oldItem.addr, oldItem.length);
	}
}

void LDP::rebuildFecList()
{
	ev << "make list of recognized FECs" << endl;
	
	FecVector oldList = fecList;
	fecList.clear();
	
	for(int i = 0; i < rt->numRoutingEntries(); i++)
	{
		// every entry in the routing table
		
		RoutingEntry *re = rt->routingEntry(i);

		// ignore multicast routes
		if(re->host.isMulticast())
			continue;

		// find out current next hop according to routing table		
		IPAddress nextHop = (re->type == RoutingEntry::DIRECT)? re->host: re->gateway;
		ASSERT(!nextHop.isUnspecified());

		FecVector::iterator it = findFecEntry(oldList, re->host, re->netmask.netmaskLength());
		
		if(it == oldList.end())
		{
			// fec didn't exist, it was just created
			fec_t newItem;
			newItem.fecid = ++maxFecid;
			newItem.addr = re->host;
			newItem.length = re->netmask.netmaskLength();
			newItem.nextHop = nextHop;
			updateFecListEntry(newItem);
			fecList.push_back(newItem);
		}
		else if(it->nextHop != nextHop)
		{
			// next hop for this FEC changed,
			it->nextHop = nextHop;
			updateFecListEntry(*it);				
			fecList.push_back(*it);
			oldList.erase(it);
		}
		else
		{
			// FEC didn't change, reusing old values
			fecList.push_back(*it);
			oldList.erase(it);
			continue;
		}
	}
		
	
	// our own addresses (XXX is it needed?)
	
	for (int i = 0; i< ift->numInterfaces(); ++i)
	{
		InterfaceEntry *ie = ift->interfaceAt(i);
		if(ie->outputPort() < 0)
			continue;
		
		FecVector::iterator it = findFecEntry(oldList, ie->ipv4()->inetAddress(), 32);
		if(it == oldList.end())
		{
			fec_t newItem;
			newItem.fecid = ++maxFecid;
			newItem.addr = ie->ipv4()->inetAddress();
			newItem.length = 32;
			newItem.nextHop = ie->ipv4()->inetAddress();
			fecList.push_back(newItem);
		}
		else
		{
			fecList.push_back(*it);
			oldList.erase(it);
		}
	}
	
	if(oldList.size() > 0)
	{
		ev << "there are " << oldList.size() << " deprecated FECs, removing them" << endl;

		FecVector::iterator it;
		for(it = oldList.begin(); it != oldList.end(); it++)
		{
			ev << "removing FEC= " << *it << endl;
			
			FecBindVector::iterator dit;
			for(dit = fecDown.begin(); dit != fecDown.end(); dit++)
			{
				if(dit->fecid != it->fecid)
					continue;
					
				ev << "sending release label=" << dit->label << " downstream to " << dit->peer << endl;
					
				sendMapping(LABEL_RELEASE, dit->peer, dit->label, it->addr, it->length);
			}
			
			FecBindVector::iterator uit;
			for(uit = fecUp.begin(); uit != fecUp.end(); uit++)
			{
				if(uit->fecid != it->fecid)
					continue;
					
				ev << "sending withdraw label=" << uit->label << " upstream to " << uit->peer << endl;
				
				sendMapping(LABEL_WITHDRAW, uit->peer, uit->label, it->addr, it->length);
				
				ev << "removing entry inLabel=" << uit->label << " from LIB" << endl;
				
				lt->removeLibEntry(uit->label);
			}
			
		}
	}
	
	// we must keep this list sorted for matching to work correctly
	// this is probably slower than it must be
	
	std::sort(fecList.begin(), fecList.end(), fecPrefixCompare);
	
}

void LDP::updateFecList(IPAddress nextHop)
{
    FecVector::iterator it;
    for(it = fecList.begin(); it != fecList.end(); it++)
    {
    	if(it->nextHop != nextHop)
    		continue;
    		
	    updateFecListEntry(*it);
    }
}

void LDP::sendHelloTo(IPAddress dest)
{
    LDPHello *hello = new LDPHello("LDP-Hello");
    hello->setLength(LDP_HEADER_BYTES);
    hello->setType(HELLO);
    hello->setSenderAddress(rt->getRouterId());
    //hello->setReceiverAddress(...);
    hello->setHoldTime(holdTime);
    //hello->setRbit(...);
    //hello->setTbit(...);

    UDPControlInfo *controlInfo = new UDPControlInfo();
    //controlInfo->setSrcAddr(rt->getRouterId());
    controlInfo->setDestAddr(dest);
    controlInfo->setSrcPort(LDP_PORT);
    controlInfo->setDestPort(LDP_PORT);
    hello->setControlInfo(controlInfo);
    
    hello->addPar("color") = LDP_HELLO_TRAFFIC;

    send(hello, "to_udp_interface");
}

void LDP::processHelloTimeout(cMessage *msg)
{
	// peer is gone
	
	unsigned int i;
	for(i = 0; i < myPeers.size(); i++)
	{
		if(myPeers[i].timeout == msg)
			break;
	}
	ASSERT(i < myPeers.size());
	
	IPAddress peerIP = myPeers[i].peerIP;
	
	ev << "peer=" << peerIP << " is gone, removing adjacency" << endl;
	
	ASSERT(!myPeers[i].timeout->isScheduled());
	delete myPeers[i].timeout;
	ASSERT(myPeers[i].socket);
	myPeers[i].socket->abort();	// should we only close?
	delete myPeers[i].socket;
	myPeers.erase(myPeers.begin() + i);
	
	ev << "removing (stale) bindings from fecDown for peer=" << peerIP << endl;
	
	FecBindVector::iterator dit;
	for(dit = fecDown.begin(); dit != fecDown.end(); dit++)
	{
		if(dit->peer != peerIP)
			continue;
			
		ev << "label=" << dit->label << endl;
		
		// send release message just in case (?)
		// what happens if peer is not really down and
		// hello messages just disappeared?
		// does the protocol recover on its own (XXX check this)
		
		fecDown.erase(dit--);
	}
	
	ev << "removing bindings from sent to peer=" << peerIP << " from fecUp" << endl;

	FecBindVector::iterator uit;
	for(uit = fecUp.begin(); uit != fecUp.end(); uit++)
	{
		if(uit->peer != peerIP)
			continue;
			
		ev << "label=" << uit->label << endl;
		
		// send withdraw message just in case (?)
		// see comment above...
		
		fecUp.erase(uit--);
	}
	
	ev << "updating fecList" << endl;
	
	updateFecList(peerIP);
	
	// update TED and routing table
	
	unsigned int index = tedmod->linkIndex(rt->getRouterId(), peerIP);
	tedmod->ted[index].state = false;
	announceLinkChange(rt->getRouterId(), peerIP);
	tedmod->rebuildRoutingTable();
}

void LDP::processLDPHello(LDPHello *msg)
{
    UDPControlInfo *controlInfo = check_and_cast<UDPControlInfo *>(msg->controlInfo());
    //IPAddress peerAddr = controlInfo->getSrcAddr().get4();
    IPAddress peerAddr = msg->senderAddress();
    int inputPort = controlInfo->getInputPort();
    delete msg;

    ev << "Received LDP Hello from " << peerAddr << ", ";

    if (peerAddr.isUnspecified() || peerAddr==rt->getRouterId())
    {
        // must be ourselves (we're also in the all-routers multicast group), ignore
        ev << "that's myself, ignore\n";
        return;
    }
    
	// mark link as working if was failed and rebuild table
	unsigned int index = tedmod->linkIndex(rt->getRouterId(), peerAddr);
	if(!tedmod->ted[index].state)
	{
		tedmod->ted[index].state = true;
		tedmod->rebuildRoutingTable();
		announceLinkChange(rt->getRouterId(), peerAddr);
	}

    // peer already in table?
    int i = findPeer(peerAddr);
    if (i!=-1)
    {
        ev << "already in my peer table, rescheduling timeout" << endl;
        ASSERT(myPeers[i].timeout);
        cancelEvent(myPeers[i].timeout);
        scheduleAt(simTime() + holdTime, myPeers[i].timeout);
        return;
    }

    // not in table, add it
    peer_info info;
    info.peerIP = peerAddr;
    info.linkInterface = ift->interfaceByPortNo(inputPort)->name();
    info.activeRole = peerAddr.getInt() > rt->getRouterId().getInt();
    info.socket = NULL;
    info.timeout = new cMessage("HelloTimeout");
    scheduleAt(simTime() + holdTime, info.timeout);
    myPeers.push_back(info);
    int peerIndex = myPeers.size()-1;

    ev << "added to peer table\n";
    ev << "We'll be " << (info.activeRole ? "ACTIVE" : "PASSIVE") << " in this session\n";

    // introduce ourselves with a Hello, then connect if we're in ACTIVE role
    sendHelloTo(peerAddr);
    if (info.activeRole)
    {
        ev << "Establishing session with it\n";
        openTCPConnectionToPeer(peerIndex);
    }
}

void LDP::openTCPConnectionToPeer(int peerIndex)
{
    TCPSocket *socket = new TCPSocket();
    socket->setOutputGate(gate("to_tcp_interface"));
    socket->setCallbackObject(this, (void*)peerIndex);
    socket->bind(rt->getRouterId(), 0);
    socketMap.addSocket(socket);
    myPeers[peerIndex].socket = socket;

    socket->connect(myPeers[peerIndex].peerIP, LDP_PORT);
}

void LDP::processMessageFromTCP(cMessage *msg)
{
    TCPSocket *socket = socketMap.findSocketFor(msg);
    if (!socket)
    {
        // not yet in socketMap, must be new incoming connection.
        // find which peer it is and register connection
        socket = new TCPSocket(msg);
        socket->setOutputGate(gate("to_tcp_interface"));

        // FIXME there seems to be some confusion here. Is it sure that
        // routerIds we use as peerAddrs are the same as IP addresses
        // the routing is based on?
        IPAddress peerAddr = socket->remoteAddress().get4();

        int i = findPeer(peerAddr);
        if (i==-1 || myPeers[i].socket)
        {
            // nothing known about this guy, or already connected: refuse
            socket->close(); // reset()?
            delete socket;
            delete msg;
            return;
        }
        myPeers[i].socket = socket;
        socket->setCallbackObject(this, (void *)i);
        socketMap.addSocket(socket);
    }

    // dispatch to socketEstablished(), socketDataArrived(), socketPeerClosed()
    // or socketFailure()
    socket->processMessage(msg);
}

void LDP::socketEstablished(int, void *yourPtr)
{
    peer_info& peer = myPeers[(long)yourPtr];
    ev << "TCP connection established with peer " << peer.peerIP << "\n";
    
    // we must update all entries with nextHop == peerIP
    updateFecList(peer.peerIP);
    
    // FIXME start LDP session setup (if we're on the active side?)
}

void LDP::socketDataArrived(int, void *yourPtr, cMessage *msg, bool)
{
    peer_info& peer = myPeers[(long)yourPtr];
    ev << "Message arrived over TCP from peer " << peer.peerIP << "\n";

    delete msg->removeControlInfo();
    processLDPPacketFromTCP(check_and_cast<LDPPacket *>(msg));
}

void LDP::socketPeerClosed(int, void *yourPtr)
{
    peer_info& peer = myPeers[(long)yourPtr];
    ev << "Peer " << peer.peerIP << " closed TCP connection\n";

    ASSERT(false);
    
/*
    // close the connection (if not already closed)
    if (socket.state()==TCPSocket::PEER_CLOSED)
    {
        ev << "remote TCP closed, closing here as well\n";
        close();
    }
*/
}

void LDP::socketClosed(int, void *yourPtr)
{
    peer_info& peer = myPeers[(long)yourPtr];
    ev << "TCP connection to peer " << peer.peerIP << " closed\n";

    ASSERT(false);
    
    // FIXME what now? reconnect after a delay?
}

void LDP::socketFailure(int, void *yourPtr, int code)
{
    peer_info& peer = myPeers[(long)yourPtr];
    ev << "TCP connection to peer " << peer.peerIP << " broken\n";
    
    ASSERT(false);

    // FIXME what now? reconnect after a delay?
}

void LDP::processLDPPacketFromTCP(LDPPacket *ldpPacket)
{
    switch (ldpPacket->type())
    {
    case HELLO:
        error("Received LDP HELLO over TCP (should arrive over UDP)");

    case ADDRESS:
        // processADDRESS(ldpPacket);
        error("Received LDP ADDRESS message, unsupported in this version");
        break;

    case ADDRESS_WITHDRAW:
        // processADDRESS_WITHDRAW(ldpPacket);
        error("LDP PROC DEBUG: Received LDP ADDRESS_WITHDRAW message, unsupported in this version");
        break;

    case LABEL_MAPPING:
        processLABEL_MAPPING(check_and_cast<LDPLabelMapping *>(ldpPacket));
        break;

    case LABEL_REQUEST:
        processLABEL_REQUEST(check_and_cast<LDPLabelRequest *>(ldpPacket));
        break;

    case LABEL_WITHDRAW:
        processLABEL_WITHDRAW(check_and_cast<LDPLabelMapping *>(ldpPacket));
        break;

    case LABEL_RELEASE:
        processLABEL_RELEASE(check_and_cast<LDPLabelMapping *>(ldpPacket));
        break;
        
	case NOTIFICATION:
		processNOTIFICATION(check_and_cast<LDPNotify*>(ldpPacket));
		break;

    default:
        error("LDP PROC DEBUG: Unrecognized LDP Message Type, type is %d", ldpPacket->type());
    }
}

IPAddress LDP::locateNextHop(IPAddress dest)
{
    // Mapping L3 IP-host of next hop to L2 peer address.

    // Lookup the routing table, rfc3036
    // "When the FEC for which a label is requested is a Prefix FEC Element or
    //  a Host Address FEC Element, the receiving LSR uses its routing table to determine
    //  its response. Unless its routing table includes an entry that exactly matches
    //  the requested Prefix or Host Address, the LSR must respond with a
    //  No Route Notification message."
    //
    // FIXME the code below (though seems like that's what the RFC refers to) doesn't work
    // -- we can't reasonably expect the destination host to be exaplicitly in an
    // LSR's routing table!!! Use simple IP routing instead. --Andras
    //
    // Wrong code:
    //int i;
    //for (i=0; i < rt->numRoutingEntries(); i++)
    //    if (rt->routingEntry(i)->host == dest)
    //        break;
    //
    //if (i == rt->numRoutingEntries())
    //    return IPAddress();  // Signal an NOTIFICATION of NO ROUTE
    //
    int portNo = rt->outputPortNo(dest);
    if (portNo==-1)
        return IPAddress();  // no route

    string iName = ift->interfaceByPortNo(portNo)->name();
    return findPeerAddrFromInterface(iName);
}

// To allow this to work, make sure there are entries of hosts for all peers

IPAddress LDP::findPeerAddrFromInterface(string interfaceName)
{
    int i = 0;
    int k = 0;
    InterfaceEntry *ie = ift->interfaceByName(interfaceName.c_str());

    RoutingEntry *anEntry;

    for (i = 0; i < rt->numRoutingEntries(); i++)
    {
        for (k = 0; k < (int)myPeers.size(); k++)
        {
            anEntry = rt->routingEntry(i);
            if (anEntry->host==myPeers[k].peerIP && anEntry->interfacePtr==ie)
            {
                return myPeers[k].peerIP;
            }
            // addresses->push_back(peerIP[k]);
        }
    }

    // Return any IP which has default route - not in routing table entries
    for (i = 0; i < (int)myPeers.size(); i++)
    {
        for (k = 0; k < rt->numRoutingEntries(); k++)
        {
            anEntry = rt->routingEntry(i);
            if (anEntry->host == myPeers[i].peerIP)
                break;
        }
        if (k == rt->numRoutingEntries())
            break;
    }

    // return the peer's address if found, unspecified address otherwise
    return i==myPeers.size() ? IPAddress() : myPeers[i].peerIP;
}

// Pre-condition: myPeers vector is finalized
string LDP::findInterfaceFromPeerAddr(IPAddress peerIP)
{
/*
    int i;
    for(unsigned int i=0;i<myPeers.size();i++)
    {
        if(myPeers[i].peerIP == peerIP)
            return string(myPeers[i].linkInterface);
    }
    return string("X");
*/
//    Rely on port index to find the interface name
    int portNo = rt->outputPortNo(peerIP);
    return ift->interfaceByPortNo(portNo)->name();

}

//bool LDP::matches(const FEC_TLV& a, const FEC_TLV& b)
//{
//	return b.addr.prefixMatches(a, b.length);
//}

LDP::FecBindVector::iterator LDP::findFecEntry(FecBindVector& fecs, int fecid, IPAddress peer)
{
	FecBindVector::iterator it;
	for(it = fecs.begin(); it != fecs.end(); it++)
	{
		if(it->fecid != fecid)
			continue;
			
		if(it->peer != peer)
			continue;
			
		break;
	}
	return it;
}

LDP::FecVector::iterator LDP::findFecEntry(FecVector& fecs, IPAddress addr, int length)
{
	FecVector::iterator it;
	for(it = fecs.begin(); it != fecs.end(); it++)
	{
		if(it->length != length)
			continue;
			
		if(it->addr != addr) // XXX compare only relevant part (?)
			continue;
			
		break;
	}
	return it;
}

void LDP::sendNotify(int status, IPAddress dest, IPAddress addr, int length)
{
	// Send NOTIFY message
	LDPNotify *lnMessage = new LDPNotify("Lb-Notify");
	lnMessage->setLength(LDP_HEADER_BYTES);
	lnMessage->setType(NOTIFICATION);
	lnMessage->setStatus(NO_ROUTE);
	lnMessage->setLength(30*8); // FIXME find out actual length
	lnMessage->setReceiverAddress(dest);
	lnMessage->setSenderAddress(rt->getRouterId());

	FEC_TLV fec;
	fec.addr = addr;
	fec.length = length;
	
	lnMessage->setFec(fec);

	sendToPeer(dest, lnMessage);
}

void LDP::sendMapping(int type, IPAddress dest, int label, IPAddress addr, int length)
{
	// Send LABEL MAPPING downstream
	LDPLabelMapping *lmMessage = new LDPLabelMapping("Lb-Mapping");
	lmMessage->setLength(LDP_HEADER_BYTES);
	lmMessage->setType(type);
	lmMessage->setLength(30*8); // FIXME find out actual length
	lmMessage->setReceiverAddress(dest);
	lmMessage->setSenderAddress(rt->getRouterId());
	lmMessage->setLabel(label);

	FEC_TLV fec;
	fec.addr = addr;
	fec.length = length;
	
	lmMessage->setFec(fec);

	sendToPeer(dest, lmMessage);
}
void LDP::processNOTIFICATION(LDPNotify *packet)
{
    FEC_TLV fec = packet->getFec();
    IPAddress srcAddr = packet->senderAddress();
    int status = packet->getStatus();
    
    // XXX FIXME NO_ROUTE processing should probably be split into two functions,
    // this is not the cleanest thing I ever wrote :)
    
    if(packet->isSelfMessage())
    {
    	// re-scheduled by ourselves
		ev << "notification retry for peer=" << srcAddr << " fec=" << fec << " status=" << status << endl;
    }
    else
    {
    	// received via network
		ev << "notification received from=" << srcAddr << " fec=" << fec << " status=" << status << endl;
    }
	
	switch(status)
	{
		case NO_ROUTE:
		{
			ev << "route does not exit on that peer" << endl;
		
			FecVector::iterator it = findFecEntry(fecList, fec.addr, fec.length);	
			if(it != fecList.end())
			{
				if(it->nextHop == srcAddr)
				{
					if(!packet->isSelfMessage())
					{
						ev << "we are still interesed in this mapping, we will retry later" << endl;
					
						scheduleAt(simTime() + 1.0 /* XXX FIXME */, packet);
						return;
					}
					else
					{
						ev << "reissuing request" << endl;
						
						sendMappingRequest(srcAddr, fec.addr, fec.length);
					}
				}
				else
					ev << "and we still recognize this FEC, but we use different next hop, forget it" << endl;
			}
			else
				ev << "and we do not recognize this any longer, forget it" << endl;
			
			break;
		}
			
		default:
			ASSERT(false);
	}
	
	delete packet;
}

void LDP::processLABEL_REQUEST(LDPLabelRequest *packet)
{
    FEC_TLV fec = packet->getFec();
    IPAddress srcAddr = packet->senderAddress();
    
    ev << "Label Request from LSR " << srcAddr << " for FEC " << fec << endl;
    
	FecVector::iterator it = findFecEntry(fecList, fec.addr, fec.length);
	if(it == fecList.end())
	{
		ev << "FEC not recognized, sending back No route message" << endl;
		
		sendNotify(NO_ROUTE, srcAddr, fec.addr, fec.length);
		
		delete packet;
		return;
	}

	// do we already have mapping for this fec from our downstream peer?
	
	//
	// XXX this code duplicates rebuildFecList
	//
	
	// does upstream have mapping from us? 
	FecBindVector::iterator uit = findFecEntry(fecUp, it->fecid, srcAddr);
	
	// shouldn't!
	ASSERT(uit == fecUp.end());
	
	// do we have mapping from downstream?
	FecBindVector::iterator dit = findFecEntry(fecDown, it->fecid, it->nextHop);
			
	// is next hop our LDP peer?
	bool ER = !peerSocketSoft(it->nextHop);
	
	ASSERT(!(ER && dit != fecDown.end())); // can't be egress and have mapping at the same time

	if(ER || dit != fecDown.end())
	{ 	
		fec_bind_t newItem;
		newItem.fecid = it->fecid;
		newItem.label = -1;
		newItem.peer = srcAddr;
		fecUp.push_back(newItem);
		uit = fecUp.end() - 1;
	}
		
	std::string inInterface = findInterfaceFromPeerAddr(srcAddr);
	std::string outInterface = findInterfaceFromPeerAddr(it->nextHop);
	
	if(ER)
	{
		// we are egress, that's easy:
		LabelOpVector outLabel = LIBTable::popLabel();
		
		uit->label = lt->installLibEntry(uit->label, inInterface, outLabel, outInterface, 0);
					
		ev << "installed (egress) LIB entry inLabel=" << uit->label << " inInterface=" << inInterface <<
				" outLabel=" << outLabel << " outInterface=" << outInterface << endl;
				
		// We are egress, let our upstream peer know
		// about it by sending back a Label Mapping message
		
		sendMapping(LABEL_MAPPING, srcAddr, uit->label, fec.addr, fec.length);
		
	}
	else if(dit != fecDown.end())
	{
		// we have mapping from DS, that's easy
		LabelOpVector outLabel = LIBTable::swapLabel(dit->label);
		uit->label = lt->installLibEntry(uit->label, inInterface, outLabel, outInterface, LDP_USER_TRAFFIC);
					
		ev << "installed LIB entry inLabel=" << uit->label << " inInterface=" << inInterface <<
				" outLabel=" << outLabel << " outInterface=" << outInterface << endl;

		// We already have a mapping for this FEC, let our upstream peer know
		// about it by sending back a Label Mapping message

		sendMapping(LABEL_MAPPING, srcAddr, uit->label, fec.addr, fec.length);
	}
	else
	{
		// no mapping from DS, mark as pending
			
		ev << "no mapping for this FEC from the downstream router, marking as pending" << endl;
		
		pending_req_t newItem;
		newItem.fecid = it->fecid;
		newItem.peer = srcAddr;
		pending.push_back(newItem);
	}
		
	delete packet;
}

void LDP::processLABEL_RELEASE(LDPLabelMapping *packet)
{
    FEC_TLV fec = packet->getFec();
    int label = packet->getLabel();
    IPAddress fromIP = packet->senderAddress();
    
    ev << "Mapping release received for label=" << label << " fec=" << fec << " from " << fromIP << endl;
    
    ASSERT(label > 0);
    
    // remove label from fecUp
    
    FecVector::iterator it = findFecEntry(fecList, fec.addr, fec.length);
    if(it == fecList.end())
    {
    	ev << "FEC no longer recognized here, ignoring" << endl;
    	delete packet;
    	return;
    }
    	
    FecBindVector::iterator uit = findFecEntry(fecUp, it->fecid, fromIP);
    if(uit == fecUp.end() || label != uit->label)
    {
    	// this is ok and may happen; e.g. we removed the mapping because downstream
    	// neighbour withdrew its mapping. we sent withdraw upstream as well and
    	// this is upstream's response
    	ev << "mapping not found among sent mappings, ignoring" << endl;
    	delete packet;
    	return;
    }
    
    ev << "removing from LIB table label=" << uit->label << endl;
    lt->removeLibEntry(uit->label);
    
    ev << "removing label from list of sent mappings" << endl;
    fecUp.erase(uit);
    
    delete packet;
}

void LDP::processLABEL_WITHDRAW(LDPLabelMapping *packet)
{
    FEC_TLV fec = packet->getFec();
    int label = packet->getLabel();
    IPAddress fromIP = packet->senderAddress();
    
    ev << "Mapping withdraw received for label=" << label << " fec=" << fec << " from " << fromIP << endl;
    
    ASSERT(label > 0);
    
    // remove label from fecDown
    
    FecVector::iterator it = findFecEntry(fecList, fec.addr, fec.length);
    if(it == fecList.end())
    {
    	ev << "matching FEC not found, ignoring withdraw message" << endl;
    	delete packet;
    	return;
    }
    
    FecBindVector::iterator dit = findFecEntry(fecDown, it->fecid, fromIP);
    
    if(dit == fecDown.end() || label != dit->label)
    {
    	ev << "matching mapping not found, ignoring withdraw message" << endl;
    	delete packet;
    	return;
    }
    
    ASSERT(dit != fecDown.end());
    ASSERT(label == dit->label);
    
    ev << "removing label from list of received mappings" << endl;
    fecDown.erase(dit);
    
    ev << "sending back relase message" << endl;
    packet->setType(LABEL_RELEASE);

    // send msg to peer over TCP
    sendToPeer(fromIP, packet);
    
    updateFecListEntry(*it);
}

void LDP::processLABEL_MAPPING(LDPLabelMapping *packet)
{
    FEC_TLV fec = packet->getFec();
    int label = packet->getLabel();
    IPAddress fromIP = packet->senderAddress();

    ev << "Label mapping label=" << label << " received for fec=" << fec << " from " << fromIP << endl;

    ASSERT(label > 0);
    
    FecVector::iterator it = findFecEntry(fecList, fec.addr, fec.length);
    ASSERT(it != fecList.end());
    
    FecBindVector::iterator dit = findFecEntry(fecDown, it->fecid, fromIP);
    ASSERT(dit == fecDown.end());
    
    // insert among received mappings
    
    fec_bind_t newItem;
    newItem.fecid = it->fecid;
    newItem.peer = fromIP;
    newItem.label = label;
    fecDown.push_back(newItem);
    
	// respond to pending requests
	
	PendingVector::iterator pit;
	for(pit = pending.begin(); pit != pending.end(); pit++)
	{
		if(pit->fecid != it->fecid)
			continue;
			
		ev << "there's pending request for this FEC from " << pit->peer << ", sending mapping" << endl;
		
		std::string inInterface = findInterfaceFromPeerAddr(pit->peer);
		std::string outInterface = findInterfaceFromPeerAddr(fromIP);
		LabelOpVector outLabel = LIBTable::swapLabel(label);
		
		fec_bind_t newItem;
		newItem.fecid = it->fecid;
		newItem.peer = pit->peer;
		newItem.label = lt->installLibEntry(-1, inInterface, outLabel, outInterface, LDP_USER_TRAFFIC);
		fecUp.push_back(newItem);
		
		ev << "installed LIB entry inLabel=" << newItem.label << " inInterface=" << inInterface <<
				" outLabel=" << outLabel << " outInterface=" << outInterface << endl;
		
		sendMapping(LABEL_MAPPING, pit->peer, newItem.label, it->addr, it->length);
		
		// remove request from the list
		pending.erase(pit--);
	}
	
	delete packet;
}

int LDP::findPeer(IPAddress peerAddr)
{
    for (PeerVector::iterator i=myPeers.begin(); i!=myPeers.end(); ++i)
        if (i->peerIP==peerAddr)
            return i-myPeers.begin();
    return -1;
}

TCPSocket *LDP::peerSocketSoft(IPAddress peerAddr)
{
    // find peer in table and return its socket
    int i = findPeer(peerAddr);
    if (i==-1 || !(myPeers[i].socket) || myPeers[i].socket->state()!=TCPSocket::CONNECTED)
    {
        // we don't have an LDP session to this peer
        return NULL;
    }
    return myPeers[i].socket;
}

TCPSocket *LDP::peerSocket(IPAddress peerAddr)
{
	TCPSocket *sock = peerSocketSoft(peerAddr);
	ASSERT(sock);
	if(!sock)
		error("No LDP session to peer %s yet", peerAddr.str().c_str());
    return sock;
}

bool LDP::lookupLabel(IPDatagram *ipdatagram, LabelOpVector& outLabel, std::string& outInterface, int& color)
{
	IPAddress destAddr = ipdatagram->destAddress();
	int protocol = ipdatagram->transportProtocol();
	
	// never match and always route via L3 if:
	
	// OSPF traffic (TED)
	if(protocol == IP_PROT_OSPF)
		return false;
		
	// LDP traffic (both discovery...
	if(protocol == IP_PROT_UDP && check_and_cast<UDPPacket*>(ipdatagram->encapsulatedMsg())->destinationPort() == LDP_PORT)
		return false;
		
	// ...and session)
	if(protocol == IP_PROT_TCP && check_and_cast<TCPSegment*>(ipdatagram->encapsulatedMsg())->destPort() == LDP_PORT)
		return false;
	if(protocol == IP_PROT_TCP && check_and_cast<TCPSegment*>(ipdatagram->encapsulatedMsg())->srcPort() == LDP_PORT)
		return false;

	// regular traffic, classify, label etc.
	
	FecVector::iterator it;
	for(it = fecList.begin(); it != fecList.end(); it++)
	{
		if(!destAddr.prefixMatches(it->addr, it->length))
			continue;
			
		ev << "FEC matched: " << *it << endl;
		
		FecBindVector::iterator dit = findFecEntry(fecDown, it->fecid, it->nextHop);
		if(dit != fecDown.end())
		{
			outLabel = LIBTable::pushLabel(dit->label);
			outInterface = findInterfaceFromPeerAddr(it->nextHop);
			color = LDP_USER_TRAFFIC;
			ev << "mapping found, outLabel=" << outLabel << ", outInterface=" << outInterface << endl;
			return true;
		}
		else
		{
			ev << "no mapping for this FEC exists" << endl;
			return false;
		}
	}
	return false;
}

void LDP::receiveChangeNotification(int category, cPolymorphic *details)
{
	Enter_Method_Silent();
	
	ASSERT(category == NF_IPv4_ROUTINGTABLE_CHANGED);
	
	ev << "routing table changed, rebuild list of known FEC" << endl;
	
	rebuildFecList();
}

void LDP::announceLinkChange(IPAddress advrouter, IPAddress linkid)
{
	TELink link;
	link.advrouter = advrouter;
	link.linkid = linkid;

	LinkNotifyMsg *msg = new LinkNotifyMsg("notify");
	msg->setLinkArraySize(1);
	msg->setLink(0, link);
	sendDirect(msg, 0.0, tedmod, "inotify");
}


