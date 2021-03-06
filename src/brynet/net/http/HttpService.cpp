#include <string>
#include <cstring>
#include <cassert>

#include <brynet/utils/SHA1.h>
#include <brynet/utils/base64.h>
#include <brynet/net/http/http_parser.h>
#include <brynet/net/http/WebSocketFormat.h>

#include <brynet/net/http/HttpService.h>

using namespace brynet::net;

HttpSession::HttpSession(TCPSession::PTR session)
{
    mSession = session;
}

TCPSession::PTR& HttpSession::getSession()
{
    return mSession;
}

const std::any& HttpSession::getUD() const
{
    return mUD;
}

void HttpSession::setUD(std::any ud)
{
    mUD = ud;
}

void HttpSession::setHttpCallback(HTTPPARSER_CALLBACK callback)
{
    mHttpRequestCallback = std::move(callback);
}

void HttpSession::setCloseCallback(CLOSE_CALLBACK callback)
{
    mCloseCallback = std::move(callback);
}

void HttpSession::setWSCallback(WS_CALLBACK callback)
{
    mWSCallback = std::move(callback);
}

void HttpSession::setWSConnected(WS_CONNECTED_CALLBACK callback)
{
    mWSConnectedCallback = std::move(callback);
}

HttpSession::HTTPPARSER_CALLBACK& HttpSession::getHttpCallback()
{
    return mHttpRequestCallback;
}

HttpSession::CLOSE_CALLBACK& HttpSession::getCloseCallback()
{
    return mCloseCallback;
}

HttpSession::WS_CALLBACK& HttpSession::getWSCallback()
{
    return mWSCallback;
}

HttpSession::WS_CONNECTED_CALLBACK& HttpSession::getWSConnectedCallback()
{
    return mWSConnectedCallback;
}

void HttpSession::send(const DataSocket::PACKET_PTR& packet, const DataSocket::PACKED_SENDED_CALLBACK& callback /* = nullptr */)
{
    mSession->send(packet, callback);
}

void HttpSession::send(const char* packet, size_t len, const DataSocket::PACKED_SENDED_CALLBACK& callback)
{
    mSession->send(packet, len, callback);
}

void HttpSession::postShutdown() const
{
    mSession->postShutdown();
}

void HttpSession::postClose() const
{
    mSession->postClose();
}

HttpSession::PTR HttpSession::Create(TCPSession::PTR session)
{
    struct make_shared_enabler : public HttpSession
    {
    public:
        make_shared_enabler(TCPSession::PTR session) : HttpSession(session)
        {}
    };

    return std::make_shared<make_shared_enabler>(session);
}

HttpService::PTR HttpService::Create()
{
    struct make_shared_enabler : public HttpService {};
    return std::make_shared<make_shared_enabler>();
}

HttpService::HttpService()
{
    mService = std::make_shared<WrapTcpService>();
}

HttpService::~HttpService()
{
    if (mListenThread != nullptr)
    {
        mListenThread->closeListenThread();
    }
    if (mService != nullptr)
    {
        mService->getService()->closeService();
    }
}

WrapTcpService::PTR HttpService::getService()
{
    return mService;
}

void HttpService::setEnterCallback(ENTER_CALLBACK callback)
{
    mOnEnter = std::move(callback);
}

void HttpService::addConnection(sock fd, 
    ENTER_CALLBACK enterCallback, 
    HttpSession::HTTPPARSER_CALLBACK responseCallback, 
    HttpSession::WS_CALLBACK wsCallback, 
    HttpSession::CLOSE_CALLBACK closeCallback,
    HttpSession::WS_CONNECTED_CALLBACK wsConnectedCallback)
{
    mService->addSession(fd, [shared_this = shared_from_this(),
        enterCallbackCapture = std::move(enterCallback),
        responseCallbackCapture = std::move(responseCallback),
        wsCallbackCapture = std::move(wsCallback),
        closeCallbackCapture = std::move(closeCallback),
        wsConnectedCallbackCapture = std::move(wsConnectedCallback)](const TCPSession::PTR& session){
        auto httpSession = HttpSession::Create(session);
        httpSession->setCloseCallback(std::move(closeCallbackCapture));
        httpSession->setWSCallback(std::move(wsCallbackCapture));
        httpSession->setHttpCallback(std::move(responseCallbackCapture));
        httpSession->setWSConnected(std::move(wsConnectedCallbackCapture));

        enterCallbackCapture(httpSession);
        shared_this->handleHttp(httpSession);
    }, false, 32*1024 * 1024);
}

void HttpService::startWorkThread(size_t workthreadnum, TcpService::FRAME_CALLBACK callback)
{
    mService->startWorkThread(workthreadnum, callback);
}

void HttpService::startListen(bool isIPV6, const std::string& ip, int port, const char *certificate /* = nullptr */, const char *privatekey /* = nullptr */)
{
    if (mListenThread == nullptr)
    {
        mListenThread = ListenThread::Create();
        mListenThread->startListen(isIPV6, ip, port, certificate, privatekey, [shared_this = shared_from_this()](sock fd){
            shared_this->mService->addSession(fd, [shared_this](const TCPSession::PTR& session){
                auto httpSession = HttpSession::Create(session);
                if (shared_this->mOnEnter != nullptr)
                {
                    shared_this->mOnEnter(httpSession);
                }
                shared_this->handleHttp(httpSession);
            }, false, 32 * 1024 * 1024);
        });
    }
}

static HTTPParser::PTR castHttpParse(const TCPSession::PTR& session)
{
    auto ud = std::any_cast<HTTPParser::PTR>(&session->getUD());
    if (ud == nullptr)
    {
        return nullptr;
    }

    return *ud;
}

void HttpService::handleHttp(const HttpSession::PTR& httpSession)
{
    /*TODO::keep alive and timeout close */
    auto& session = httpSession->getSession();
    session->setUD(std::make_shared<HTTPParser>(HTTP_BOTH));

    session->setCloseCallback([httpSession](const TCPSession::PTR& session){
        auto httpParser = castHttpParse(session);
        auto& tmp = httpSession->getCloseCallback();
        if (tmp != nullptr)
        {
            tmp(httpSession);
        }
    });

    session->setDataCallback([shared_this = shared_from_this(), httpSession](const TCPSession::PTR& session, const char* buffer, size_t len){
        size_t retlen = 0;

        auto httpParser = castHttpParse(session);
        assert(httpParser != nullptr);
        if (httpParser == nullptr)
        {
            return retlen;
        }

        if (httpParser->isWebSocket())
        {
            const char* parse_str = buffer;
            size_t leftLen = len;

            auto& cacheFrame = httpParser->getWSCacheFrame();
            auto& parseString = httpParser->getWSParseString();

            while (leftLen > 0)
            {
                parseString.clear();

                auto opcode = WebSocketFormat::WebSocketFrameType::ERROR_FRAME; /*TODO::opcode是否回传给回调函数*/
                size_t frameSize = 0;
                bool isFin = false;
                if (WebSocketFormat::wsFrameExtractBuffer(parse_str, leftLen, parseString, opcode, frameSize, isFin))
                {
                    if (isFin && (opcode == WebSocketFormat::WebSocketFrameType::TEXT_FRAME || opcode == WebSocketFormat::WebSocketFrameType::BINARY_FRAME))
                    {
                        if (!cacheFrame.empty())
                        {
                            cacheFrame += parseString;
                            parseString = std::move(cacheFrame);
                            cacheFrame.clear();
                        }

                        auto& wsCallback = httpSession->getWSCallback();
                        if (wsCallback != nullptr)
                        {
                            wsCallback(httpSession, opcode, parseString);
                        }
                    }
                    else if (opcode == WebSocketFormat::WebSocketFrameType::CONTINUATION_FRAME)
                    {
                        cacheFrame += parseString;
                        parseString.clear();
                    }
                    else if (opcode == WebSocketFormat::WebSocketFrameType::PING_FRAME ||
                            opcode == WebSocketFormat::WebSocketFrameType::PONG_FRAME ||
                            opcode == WebSocketFormat::WebSocketFrameType::CLOSE_FRAME)
                    {
                        auto& wsCallback = httpSession->getWSCallback();
                        if (wsCallback != nullptr)
                        {
                            wsCallback(httpSession, opcode, parseString);
                        }
                    }
                    else
                    {
                        assert(false);
                    }

                    leftLen -= frameSize;
                    retlen += frameSize;
                    parse_str += frameSize;
                }
                else
                {
                    break;
                }
            }
        }
        else
        {
            retlen = httpParser->tryParse(buffer, len);
            if (httpParser->isCompleted())
            {
                if (httpParser->isWebSocket())
                {
                    if (httpParser->hasKey("Sec-WebSocket-Key"))
                    {
                        auto response = WebSocketFormat::wsHandshake(httpParser->getValue("Sec-WebSocket-Key"));
                        session->send(response.c_str(), response.size());
                    }
                    
                    auto& wsConnectedCallback = httpSession->getWSConnectedCallback();
                    if (wsConnectedCallback != nullptr)
                    {
                        wsConnectedCallback(httpSession, *httpParser);
                    }
                }
                else
                {
                    auto& httpCallback = httpSession->getHttpCallback();
                    if (httpCallback != nullptr)
                    {
                        httpCallback(*httpParser, httpSession);
                    }
                    if (httpParser->isKeepAlive())
                    {
                        /*清除本次http报文数据，为下一次http报文准备*/
                        httpParser->clearParse();
                    }
                }
            }
        }

        return retlen;
    });
}
