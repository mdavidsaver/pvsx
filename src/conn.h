/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
#ifndef CONN_H
#define CONN_H

#include "evhelper.h"
#include "dataimpl.h"
#include "utilpvt.h"

namespace pvxs {
namespace impl {

struct ConnBase
{
    const SockAddr peerAddr;
    const std::string peerName;
protected:
    evbufferevent bev;
public:
    TypeStore rxRegistry;
    /* Flag if some received delta could not be decoded due to
     * a non-existent IOID, which *may* leave this rxRegistry out
     * of sync with the peer (if it contains Variant Unions).
     * We can't know whether this is the case.
     * Failing soft here may lead to failures decoding future replies.
     * We could force close the Connection here to be "safe".
     * However, we assume the such usage of Variant is relatively rare
     */
    bool rxRegistryDirty = false;

    const bool isClient;
    bool sendBE;
    bool peerBE;
    bool expectSeg;
    uint8_t peerVersion;

    uint8_t segCmd;
    evbuf segBuf, txBody;

    size_t statTx{}, statRx{};
    size_t readahead{};

    enum {
        Holdoff,
        Connecting,
        Connected,
        Disconnected,
    } state;

    ConnBase(bool isClient, bool sendBE, evbufferevent &&bev, const SockAddr& peerAddr);
    ConnBase(const ConnBase&) = delete;
    ConnBase& operator=(const ConnBase&) = delete;
    virtual ~ConnBase();

    const char* peerLabel() const;

    size_t enqueueTxBody(pva_app_msg_t cmd);

    bufferevent* connection() { return bev.get(); }

    void connect(evbufferevent &&bev);
    void disconnect();

protected:
#define CASE(Op) virtual void handle_##Op();
    CASE(ECHO);
    CASE(CONNECTION_VALIDATION);
    CASE(CONNECTION_VALIDATED);
    CASE(SEARCH);
    CASE(SEARCH_RESPONSE);
    CASE(AUTHNZ);

    CASE(CREATE_CHANNEL);
    CASE(DESTROY_CHANNEL);

    CASE(GET);
    CASE(PUT);
    CASE(PUT_GET);
    CASE(MONITOR);
    CASE(RPC);
    CASE(CANCEL_REQUEST);
    CASE(DESTROY_REQUEST);
    CASE(GET_FIELD);

    CASE(MESSAGE);
#undef CASE

    virtual std::shared_ptr<ConnBase> self_from_this() =0;
    virtual void cleanup() =0;
    virtual void bevEvent(short events);
    virtual void bevRead();
    virtual void bevWrite();
    static void bevEventS(struct bufferevent *bev, short events, void *ptr);
    static void bevReadS(struct bufferevent *bev, void *ptr);
    static void bevWriteS(struct bufferevent *bev, void *ptr);
};

} // namespace impl
} // namespace pvxs

#endif // CONN_H
