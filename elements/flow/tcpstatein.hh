#ifndef CLICK_TCPSTATEIN_HH
#define CLICK_TCPSTATEIN_HH
#include <click/config.h>
#include <click/flowelement.hh>
#include <click/multithread.hh>
#include <click/hashtablemp.hh>
#include <click/allocator.hh>
#include <click/glue.hh>
#include <click/vector.hh>
#include <click/ipflowid.hh>
CLICK_DECLS

#include <click/hashtablemp.hh>

CLICK_DECLS

struct TCPStateCommon {
    TCPStateCommon() : closing(false) {
        ref = 0;
    }
    atomic_uint32_t ref; //Reference count
    bool closing; //Has one side started to close?
    uint32_t _pad[2];
};


/**
 * NAT Entries for the mapping side : a mapping to the original port
 * and a flag to know if the mapping has been seen on this side.
 */
struct TCPStateEntry {
    TCPStateCommon* ref;
    bool fin_seen;
};

typedef HashTableMP<IPFlowID,TCPStateCommon*> TCPStateHashtable; //Table used to pass the mapping from the mapper to the reverse

#define TCP_STATE_FLOW_TIMEOUT 16 * 1000 //Flow timeout

/**
 * Efficient MiddleClick-based TCP State machine
 *
 * Working is similar to FLowIPNat
 */
class TCPStateIN : public FlowStateElement<TCPStateIN,TCPStateEntry> {

public:

    TCPStateIN() CLICK_COLD;
    ~TCPStateIN() CLICK_COLD;

    const char *class_name() const		{ return "TCPStateIN"; }
    const char *port_count() const		{ return "1/1"; }
    const char *processing() const		{ return PUSH; }

    //TCP only for now, just to reuse the macro but nothing prevents UDP
    FLOW_ELEMENT_DEFINE_SESSION_CONTEXT("12/0/ffffffff:HASH-3 16/0/ffffffff:HASH-3 22/0/ffff 20/0/ffff:ARRAY", FLOW_TCP);

    int configure(Vector<String> &, ErrorHandler *) CLICK_COLD;
    int initialize(ErrorHandler *errh);

    static const int timeout = TCP_STATE_FLOW_TIMEOUT;
    
    bool new_flow(TCPStateEntry*, Packet*);
    void release_flow(TCPStateEntry*);

    void push_batch(int, TCPStateEntry*, PacketBatch *);
private:
    pool_allocator_mt<TCPStateCommon,false,16384> _pool;

private:
    bool _accept_nonsyn;
    TCPStateHashtable _map;
    TCPStateIN* _return;
};

CLICK_ENDDECLS
#endif