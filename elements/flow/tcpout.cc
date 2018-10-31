#include <click/config.h>
#include <click/router.hh>
#include <click/args.hh>
#include <click/error.hh>
#include <clicknet/ip.h>
#include <clicknet/tcp.h>
#include "tcpin.hh"
#include "tcpout.hh"
#include "ipelement.hh"

CLICK_DECLS

TCPOut::TCPOut() : inElement(NULL),_readonly(false), _allow_resize(false),_checksum(true)
{

}

int TCPOut::configure(Vector<String> &conf, ErrorHandler *errh)
{
    if(Args(conf, this, errh)
        .read_p("READONLY", _readonly)
        .read("CHECKSUM", _checksum)
        .complete() < 0)
            return -1;
    return 0;
}

int
TCPOut::initialize(ErrorHandler *errh) {
    if (maxModificationLevel(0) & MODIFICATION_RESIZE) {
        click_chatter("%p{element} Flow resizing support enabled",this);
        _allow_resize = true;
        if (_readonly) {
            return errh->error("Cannot modify packets in read-only mode !");
        }
    }
    return 0;
}

void TCPOut::push_batch(int port, PacketBatch* flow)
{
    auto fcb_in = inElement->fcb_data();
    auto fnt = [this,fcb_in](Packet* p) -> Packet* {
/*        if(!checkConnectionClosed(p))
        {
            p->kill();
            return NULL;
        }*/

        if (_allow_resize) {
            WritablePacket *packet = p->uniqueify();
            fcb_in->common->lock.acquire();

            bool hasModificationList = inElement->hasModificationList(packet);

            ByteStreamMaintainer &byteStreamMaintainer = fcb_in->common->maintainers[getFlowDirection()];
            ModificationList *modList = NULL;

            if(hasModificationList)
                modList = inElement->getModificationList(packet);

            // Update the sequence number (according to the modifications made on previous packets)
            tcp_seq_t prevSeq = getSequenceNumber(packet);
            tcp_seq_t newSeq =  byteStreamMaintainer.mapSeq(prevSeq);
            if (inElement->_verbose)
                click_chatter("Map SEQ %lu -> %lu", prevSeq, newSeq);
            bool seqModified = false;
            bool ackModified = false;
            tcp_seq_t prevAck = getAckNumber(packet);
            tcp_seq_t prevLastAck = 0;
            bool prevLastAckSet = false;

            if(byteStreamMaintainer.isLastAckSentSet())
            {
                prevLastAck = byteStreamMaintainer.getLastAckSent();
                prevLastAckSet = true;
            }

            if(prevSeq != newSeq)
            {
                setSequenceNumber(packet, newSeq);
                seqModified = true;
            }

            // Update the last sequence number seen
            // This number is used when crafting ACKs
            byteStreamMaintainer.setLastSeqSent(newSeq);

            // Update the window size
            byteStreamMaintainer.setWindowSize(getWindowSize(packet));

            // Update the value of the last ACK sent
            if(isAck(packet))
            {
                byteStreamMaintainer.setLastAckSent(prevAck);

                // Ensure that the value of the ACK is not below the last ACKed position
                // This solves the following problem:
                // - We ACK a packet manually for any reason
                // -> The "manual" ACK is lost
                //click_chatter("Ack %lu -> %lu", prevAck,  byteStreamMaintainer.getLastAckSent());
                setAckNumber(packet, byteStreamMaintainer.getLastAckSent());

                if(getAckNumber(packet) != prevAck)
                    ackModified = true;
            }

            // Check the length to see if bytes were added or removed
            uint16_t initialLength = packetTotalLength(packet);
            uint16_t currentLength = (uint16_t)packet->length() - getIPHeaderOffset(packet);
            int offsetModification = -(initialLength - currentLength);
            uint32_t prevPayloadSize = getPayloadLength(packet);

            // Update the "total length" field in the IP header (required to compute the
            // tcp checksum as it is in the pseudo header)
            setPacketTotalLength(packet, initialLength + offsetModification);

            // Check if the ModificationList has to be committed
            if(hasModificationList)
            {
                // We know that the packet has been modified and its size has changed
                modList->commit(fcb_in->common->maintainers[getFlowDirection()]);

                // Check if the full content of the packet has been removed
                if(getPayloadLength(packet) == 0)
                {
                    uint32_t saddr = getDestinationAddress(packet);
                    uint32_t daddr = getSourceAddress(packet);
                    uint16_t sport = getDestinationPort(packet);
                    uint16_t dport = getSourcePort(packet);
                    // The SEQ value is the initial ACK value in the packet sent
                    // by the source.
                    tcp_seq_t seq = getInitialAck(packet);

                    // The ACK is the sequence number sent by the source
                    // to which we add the old size of the payload to acknowledge it
                    tcp_seq_t ack = prevSeq + prevPayloadSize;

                    if(isFin(packet) || isSyn(packet))
                        ack++;

                    // Craft and send the ack
                    sendAck(fcb_in->common->maintainers[getOppositeFlowDirection()], saddr, daddr,
                        sport, dport, seq, ack);

                    // Even if the packet is empty it can still contain relevant
                    // information (significant ACK value or another flag)
                    if(isJustAnAck(packet))
                    {
                        // Check if the ACK of the packet was significant or not
                        if(prevLastAckSet && SEQ_LEQ(prevAck, prevLastAck))
                        {
                            // If this is not the case, drop the packet as it
                            // does not contain any relevant information
                            // (And anyway it would be considered as a duplicate ACK)
                            click_chatter("Killing useless ACK");
                            packet->kill();
                            fcb_in->common->lock.release();
                            return NULL;
                        }
                    } 
                }
            }

/*                if(prevLastAckSet && SEQ_LEQ(prevAck, prevLastAck)) {
                            click_chatter("ACK lower than already sent");
                }*/

            fcb_in->common->lock.release();

            if (!_checksum)
                resetTCPChecksum(packet);
            else
                computeTCPChecksum(packet);
            return packet;
        } else {
            tcp_seq_t seq = getSequenceNumber(p);
            tcp_seq_t ack = getAckNumber(p);
            uint16_t winSize = getWindowSize(p);
            fcb_in->common->lock.acquire();

            // Update the last sequence number seen
            // This number is used when crafting ACKs
            ByteStreamMaintainer &byteStreamMaintainer = fcb_in->common->maintainers[getFlowDirection()];
            byteStreamMaintainer.setLastSeqSent(seq);

            // Update the window size
            byteStreamMaintainer.setWindowSize(winSize);

            // Update the value of the last ACK sent
            if(isAck(p))
            {
                byteStreamMaintainer.setLastAckSent(ack);
            }
            fcb_in->common->lock.release();

            if (!_readonly) {
                // Recompute the checksum
                WritablePacket *packet = p->uniqueify();
                p = packet;
                if (!_checksum)
                    resetTCPChecksum(packet);
                else
                    computeTCPChecksum(packet);
            }
            return p;
        }
    };
    EXECUTE_FOR_EACH_PACKET(fnt, flow);

    //Release FCB if we are now closing
    TCPState::Value state = fcb_in->common->state; //Read-only for fast path
    if ((state == TCPState::BEING_CLOSED_GRACEFUL_2 ||
            state == TCPState::CLOSED)) {
//            click_chatter("RELEASING FCB STATE");
            inElement->releaseFCBState();
    }

    output(0).push_batch(flow);
}

void TCPOut::sendAck(ByteStreamMaintainer &maintainer, uint32_t saddr, uint32_t daddr,
    uint16_t sport, uint16_t dport, tcp_seq_t seq, tcp_seq_t ack, bool force)
{
    //click_chatter("Gen ack");
    if(noutputs() < 2)
    {
        click_chatter("Warning: trying to send an ack on a TCPOut with only 1 output. How could I send an ACK to the source ?");
        return;
    }


    // Check if the ACK does not bring any additional information
    if(!force && maintainer.isLastAckSentSet() && SEQ_LEQ(ack, maintainer.getLastAckSent())) {
        if (inElement->_verbose)
            click_chatter("Ack not sent, no new knowledge");
        return;
    }

    // Update the number of the last ack sent for the other side
    maintainer.setLastAckSent(ack);

    // Ensure that the sequence number of the packet is not below
    // a sequence number sent before by the other side
    if(maintainer.isLastSeqSentSet() && SEQ_LT(seq, maintainer.getLastSeqSent()))
        seq = maintainer.getLastSeqSent();

    uint16_t winSize = maintainer.getWindowSize();

    // The packet is now empty, we discard it and send an ACK directly to the source

    // Craft the packet
    Packet* forged = forgePacket(saddr, daddr, sport, dport, seq, ack, winSize, TH_ACK);

    //Send it on the second output
    #if HAVE_BATCH
        PacketBatch *batch =  PacketBatch::make_from_packet(forged);
        output_push_batch(1, batch);
    #else
        output(1).push(forged);
    #endif
}

void TCPOut::sendClosingPacket(ByteStreamMaintainer &maintainer, uint32_t saddr, uint32_t daddr,
    uint16_t sport, uint16_t dport, tcp_seq_t seq, tcp_seq_t ack, bool graceful)
{
    if(noutputs() < 2)
    {
        click_chatter("Warning: trying to send an FIN or RST packet on a TCPOut with only 1 output");
        return;
    }

    // Update the number of the last ack sent for the other side
    maintainer.setLastAckSent(ack);

    if(maintainer.isLastSeqSentSet() && SEQ_LT(seq, maintainer.getLastSeqSent()))
        seq = maintainer.getLastSeqSent();

    uint16_t winSize = maintainer.getWindowSize();

    uint8_t flag = TH_ACK;

    if(graceful)
    {
        flag = flag | TH_FIN;
        // Ensure that further packets will have seq + 1 (for the FIN flag) as a
        // sequence number
        maintainer.setLastSeqSent(seq + 1);
    }
    else
        flag = flag | TH_RST;

    // Craft the packet
    Packet* forged = forgePacket(saddr, daddr, sport, dport, seq, ack, winSize, flag);

    //Send it on the second output
    #if HAVE_BATCH
        PacketBatch *batch =  PacketBatch::make_from_packet(forged);
        output_push_batch(1, batch);
    #else
        output(1).push(forged);
    #endif
}

int TCPOut::setInElement(TCPIn* inElement, ErrorHandler* errh)
{
    this->inElement = inElement;
    inElement->add_remote_element(this);
    return 0;
}

bool TCPOut::checkConnectionClosed(Packet *packet)
{
    auto fcb_in = inElement->fcb_data();

    TCPState::Value state = fcb_in->common->state; //Read-only for fast path

    return state == TCPState::CLOSED;

}

void TCPOut::setFlowDirection(unsigned int flowDirection)
{
    this->flowDirection = flowDirection;
}

unsigned int TCPOut::getFlowDirection()
{
    return flowDirection;
}

unsigned int TCPOut::getOppositeFlowDirection()
{
    return (1 - flowDirection);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(TCPElement)
EXPORT_ELEMENT(TCPOut)
ELEMENT_MT_SAFE(TCPOut)
