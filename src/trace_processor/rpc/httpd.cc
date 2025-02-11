/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_HTTPD)

#include "src/trace_processor/rpc/httpd.h"

#include <map>
#include <string>

#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/rpc/rpc.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {

constexpr char kBindPort[] = "9001";
constexpr size_t kOmitContentLength = static_cast<size_t>(-1);

// Sets the Access-Control-Allow-Origin: $origin on the following origins.
// This affects only browser clients that use CORS. Other HTTP clients (e.g. the
// python API) don't look at CORS headers.
const char* kAllowedCORSOrigins[] = {
    "https://ui.perfetto.dev",
    "http://localhost:10000",
    "http://127.0.0.1:10000",
};

// 32 MiB payload + 128K for HTTP headers.
constexpr size_t kMaxRequestSize = (32 * 1024 + 128) * 1024;

// Owns the socket and data for one HTTP client connection.
struct Client {
  Client(std::unique_ptr<base::UnixSocket> s)
      : sock(std::move(s)),
        rxbuf(base::PagedMemory::Allocate(kMaxRequestSize)) {}
  size_t rxbuf_avail() { return rxbuf.size() - rxbuf_used; }

  std::unique_ptr<base::UnixSocket> sock;
  base::PagedMemory rxbuf;
  size_t rxbuf_used = 0;
};

struct HttpRequest {
  base::StringView method;
  base::StringView uri;
  base::StringView origin;
  base::StringView body;
  int id = 0;
};

class HttpServer : public base::UnixSocket::EventListener {
 public:
  explicit HttpServer(std::unique_ptr<TraceProcessor>);
  ~HttpServer() override;
  void Run(const char*, const char*);

  // This is non-null only while serving an HTTP request.
  Client* active_client() { return active_client_; }

 private:
  size_t ParseOneHttpRequest(Client* client);
  void HandleRequest(Client*, const HttpRequest&);
  void ServeHelpPage(Client*);

  void OnNewIncomingConnection(base::UnixSocket*,
                               std::unique_ptr<base::UnixSocket>) override;
  void OnConnect(base::UnixSocket* self, bool connected) override;
  void OnDisconnect(base::UnixSocket* self) override;
  void OnDataAvailable(base::UnixSocket* self) override;

  Rpc trace_processor_rpc_;
  base::UnixTaskRunner task_runner_;
  std::unique_ptr<base::UnixSocket> sock4_;
  std::unique_ptr<base::UnixSocket> sock6_;
  std::list<Client> clients_;
  Client* active_client_ = nullptr;
  bool origin_error_logged_ = false;
};

HttpServer* g_httpd_instance;

void Append(std::vector<char>& buf, const char* str) {
  buf.insert(buf.end(), str, str + strlen(str));
}

void Append(std::vector<char>& buf, const std::string& str) {
  buf.insert(buf.end(), str.begin(), str.end());
}

void HttpReply(base::UnixSocket* sock,
               const char* http_code,
               std::initializer_list<const char*> headers = {},
               const uint8_t* content = nullptr,
               size_t content_length = 0) {
  std::vector<char> response;
  response.reserve(4096);
  Append(response, "HTTP/1.1 ");
  Append(response, http_code);
  Append(response, "\r\n");
  for (const char* hdr : headers) {
    if (strlen(hdr) == 0)
      continue;
    Append(response, hdr);
    Append(response, "\r\n");
  }
  if (content_length != kOmitContentLength) {
    Append(response, "Content-Length: ");
    Append(response, std::to_string(content_length));
    Append(response, "\r\n");
  }
  Append(response, "\r\n");                      // End-of-headers marker.
  sock->Send(response.data(), response.size());  // Send response headers.
  if (content_length > 0 && content_length != kOmitContentLength)
    sock->Send(content, content_length);  // Send response payload.
}

void ShutdownBadRequest(base::UnixSocket* sock, const char* reason) {
  HttpReply(sock, "500 Bad Request", {},
            reinterpret_cast<const uint8_t*>(reason), strlen(reason));
  sock->Shutdown(/*notify=*/true);
}

HttpServer::HttpServer(std::unique_ptr<TraceProcessor> preloaded_instance)
    : trace_processor_rpc_(std::move(preloaded_instance)) {}
HttpServer::~HttpServer() = default;

void HttpServer::Run(const char* kBindAddr4, const char* kBindAddr6) {
  PERFETTO_ILOG("[HTTP] Starting RPC server on %s and %s", kBindAddr4,
                kBindAddr6);
  PERFETTO_LOG(
      "[HTTP] This server can be used by reloading https://ui.perfetto.dev and "
      "clicking on YES on the \"Trace Processor native acceleration\" dialog "
      "or through the Python API (see "
      "https://perfetto.dev/docs/analysis/trace-processor#python-api).");

  sock4_ = base::UnixSocket::Listen(kBindAddr4, this, &task_runner_,
                                    base::SockFamily::kInet,
                                    base::SockType::kStream);
  bool ipv4_listening = sock4_ && sock4_->is_listening();
  if (!ipv4_listening) {
    PERFETTO_ILOG("Failed to listen on IPv4 socket");
  }

  sock6_ = base::UnixSocket::Listen(kBindAddr6, this, &task_runner_,
                                    base::SockFamily::kInet6,
                                    base::SockType::kStream);
  bool ipv6_listening = sock6_ && sock6_->is_listening();
  if (!ipv6_listening) {
    PERFETTO_ILOG("Failed to listen on IPv6 socket");
  }

  PERFETTO_CHECK(ipv4_listening || ipv6_listening);

  task_runner_.Run();
}

void HttpServer::OnNewIncomingConnection(
    base::UnixSocket*,
    std::unique_ptr<base::UnixSocket> sock) {
  PERFETTO_LOG("[HTTP] New connection");
  clients_.emplace_back(std::move(sock));
}

void HttpServer::OnConnect(base::UnixSocket*, bool) {}

void HttpServer::OnDisconnect(base::UnixSocket* sock) {
  PERFETTO_LOG("[HTTP] Client disconnected");
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if (it->sock.get() == sock) {
      clients_.erase(it);
      return;
    }
  }
  PERFETTO_DFATAL("[HTTP] untracked client in OnDisconnect()");
}

void HttpServer::OnDataAvailable(base::UnixSocket* sock) {
  Client* client = nullptr;
  for (auto it = clients_.begin(); it != clients_.end() && !client; ++it)
    client = (it->sock.get() == sock) ? &*it : nullptr;
  PERFETTO_CHECK(client);

  char* rxbuf = reinterpret_cast<char*>(client->rxbuf.Get());
  for (;;) {
    size_t avail = client->rxbuf_avail();
    PERFETTO_CHECK(avail <= kMaxRequestSize);
    if (avail == 0)
      return ShutdownBadRequest(sock, "Request body too big");
    size_t rsize = sock->Receive(&rxbuf[client->rxbuf_used], avail);
    client->rxbuf_used += rsize;
    if (rsize == 0 || client->rxbuf_avail() == 0)
      break;
  }

  // At this point |rxbuf| can contain a partial HTTP request, a full one or
  // more (in case of HTTP Keepalive pipelining).
  for (;;) {
    active_client_ = client;
    size_t bytes_consumed = ParseOneHttpRequest(client);
    active_client_ = nullptr;
    if (bytes_consumed == 0)
      break;
    memmove(rxbuf, &rxbuf[bytes_consumed], client->rxbuf_used - bytes_consumed);
    client->rxbuf_used -= bytes_consumed;
  }
}

// Parses the HTTP request and invokes HandleRequest(). It returns the size of
// the HTTP header + body that has been processed or 0 if there isn't enough
// data for a full HTTP request in the buffer.
size_t HttpServer::ParseOneHttpRequest(Client* client) {
  auto* rxbuf = reinterpret_cast<char*>(client->rxbuf.Get());
  base::StringView buf_view(rxbuf, client->rxbuf_used);
  size_t pos = 0;
  size_t body_offset = 0;
  size_t body_size = 0;
  bool has_parsed_first_line = false;
  HttpRequest http_req;

  // This loop parses the HTTP request headers and sets the |body_offset|.
  for (;;) {
    size_t next = buf_view.find("\r\n", pos);
    size_t col;
    if (next == std::string::npos)
      break;

    if (!has_parsed_first_line) {
      // Parse the "GET /xxx HTTP/1.1" line.
      has_parsed_first_line = true;
      size_t space = buf_view.find(' ');
      if (space == std::string::npos || space + 2 >= client->rxbuf_used) {
        ShutdownBadRequest(client->sock.get(), "Malformed HTTP request");
        return 0;
      }
      http_req.method = buf_view.substr(0, space);
      size_t uri_size = buf_view.find(' ', space + 1) - space - 1;
      http_req.uri = buf_view.substr(space + 1, uri_size);
    } else if (next == pos) {
      // The CR-LF marker that separates headers from body.
      body_offset = next + 2;
      break;
    } else if ((col = buf_view.find(':', pos)) < next) {
      // Parse HTTP headers. They look like: "Content-Length: 1234".
      auto hdr_name = buf_view.substr(pos, col - pos);
      auto hdr_value = buf_view.substr(col + 2, next - col - 2);
      if (hdr_name.CaseInsensitiveEq("content-length")) {
        body_size = static_cast<size_t>(atoi(hdr_value.ToStdString().c_str()));
      } else if (hdr_name.CaseInsensitiveEq("origin")) {
        http_req.origin = hdr_value;
      } else if (hdr_name.CaseInsensitiveEq("x-seq-id")) {
        http_req.id = atoi(hdr_value.ToStdString().c_str());
      }
    }
    pos = next + 2;
  }

  // If we have a full header but not yet the full body, return and try again
  // next time we receive some more data.
  size_t http_req_size = body_offset + body_size;
  if (!body_offset || client->rxbuf_used < http_req_size)
    return 0;

  http_req.body = base::StringView(&rxbuf[body_offset], body_size);
  HandleRequest(client, http_req);
  return http_req_size;
}

void HttpServer::HandleRequest(Client* client, const HttpRequest& req) {
  if (req.uri == "/") {
    // If a user tries to open http://127.0.0.1:9001/ show a minimal help page.
    return ServeHelpPage(client);
  }

  static int last_req_id = 0;
  if (req.id) {
    if (last_req_id && req.id != last_req_id + 1 && req.id != 1)
      PERFETTO_ELOG("HTTP Request out of order");
    last_req_id = req.id;
  }

  PERFETTO_LOG("[HTTP] %04d %s %s (body: %zu bytes).", req.id,
               req.method.ToStdString().c_str(), req.uri.ToStdString().c_str(),
               req.body.size());

  std::string allow_origin_hdr;
  for (const char* allowed_origin : kAllowedCORSOrigins) {
    if (req.origin != base::StringView(allowed_origin))
      continue;
    allow_origin_hdr =
        "Access-Control-Allow-Origin: " + req.origin.ToStdString();
    break;
  }
  if (allow_origin_hdr.empty() && !origin_error_logged_) {
    origin_error_logged_ = true;
    PERFETTO_ELOG(
        "The HTTP origin \"%s\" is not trusted, no Access-Control-Allow-Origin "
        "will be emitted. If this request comes from a browser it will fail. "
        "For the list of allowed origins see kAllowedCORSOrigins.",
        req.origin.ToStdString().c_str());
  }

  // This is the default. Overridden by the /query handler for chunked replies.
  char transfer_encoding_hdr[255] = "Transfer-Encoding: identity";
  std::initializer_list<const char*> headers = {
      "Connection: Keep-Alive",                //
      "Cache-Control: no-cache",               //
      "Keep-Alive: timeout=5, max=1000",       //
      "Content-Type: application/x-protobuf",  //
      "Vary: Origin",                          //
      transfer_encoding_hdr,                   //
      allow_origin_hdr.c_str(),
  };

  if (req.method == "OPTIONS") {
    // CORS headers.
    return HttpReply(client->sock.get(), "204 No Content",
                     {
                         "Access-Control-Allow-Methods: POST, GET, OPTIONS",
                         "Access-Control-Allow-Headers: *",
                         "Access-Control-Max-Age: 86400",
                         "Vary: Origin",
                         allow_origin_hdr.c_str(),
                     });
  }

  if (req.uri == "/rpc") {
    // Start the chunked reply.
    base::StringCopy(transfer_encoding_hdr, "Transfer-Encoding: chunked",
                     sizeof(transfer_encoding_hdr));
    base::UnixSocket* cli_sock = client->sock.get();
    HttpReply(cli_sock, "200 OK", headers, nullptr, kOmitContentLength);

    static auto resp_fn = [](const void* data, uint32_t len) {
      char chunk_hdr[32];
      auto hdr_len = static_cast<size_t>(sprintf(chunk_hdr, "%x\r\n", len));
      auto* http_client = g_httpd_instance->active_client();
      PERFETTO_CHECK(http_client);
      if (data == nullptr) {
        // Unrecoverable RPC error case.
        http_client->sock->Send("0\r\n\r\n", 5);
        http_client->sock->Shutdown(/*notify=*/true);
        return;
      }
      http_client->sock->Send(chunk_hdr, hdr_len);
      http_client->sock->Send(data, len);
      http_client->sock->Send("\r\n", 2);
    };

    trace_processor_rpc_.SetRpcResponseFunction(resp_fn);
    trace_processor_rpc_.OnRpcRequest(req.body.data(), req.body.size());
    trace_processor_rpc_.SetRpcResponseFunction(nullptr);

    // Terminate chunked stream.
    cli_sock->Send("0\r\n\r\n", 5);
    return;
  }

  if (req.uri == "/parse") {
    trace_processor_rpc_.Parse(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    return HttpReply(client->sock.get(), "200 OK", headers);
  }

  if (req.uri == "/notify_eof") {
    trace_processor_rpc_.NotifyEndOfFile();
    return HttpReply(client->sock.get(), "200 OK", headers);
  }

  if (req.uri == "/restore_initial_tables") {
    trace_processor_rpc_.RestoreInitialTables();
    return HttpReply(client->sock.get(), "200 OK", headers);
  }

  // New endpoint, returns data in batches using chunked transfer encoding.
  // The batch size is determined by |cells_per_batch_| and
  // |batch_split_threshold_| in query_result_serializer.h.
  // This is temporary, it will be switched to WebSockets soon.
  if (req.uri == "/query") {
    std::vector<uint8_t> response;

    // Start the chunked reply.
    base::StringCopy(transfer_encoding_hdr, "Transfer-Encoding: chunked",
                     sizeof(transfer_encoding_hdr));
    base::UnixSocket* cli_sock = client->sock.get();
    HttpReply(cli_sock, "200 OK", headers, nullptr, kOmitContentLength);

    // |on_result_chunk| will be called nested within the same callstack of the
    // rpc.Query() call. No further calls will be made once Query() returns.
    auto on_result_chunk = [&](const uint8_t* buf, size_t len, bool has_more) {
      PERFETTO_DLOG("Sending response chunk, len=%zu eof=%d", len, !has_more);
      char chunk_hdr[32];
      auto hdr_len = static_cast<size_t>(sprintf(chunk_hdr, "%zx\r\n", len));
      cli_sock->Send(chunk_hdr, hdr_len);
      cli_sock->Send(buf, len);
      cli_sock->Send("\r\n", 2);
      if (!has_more) {
        hdr_len = static_cast<size_t>(sprintf(chunk_hdr, "0\r\n\r\n"));
        cli_sock->Send(chunk_hdr, hdr_len);
      }
    };
    trace_processor_rpc_.Query(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size(),
        on_result_chunk);
    return;
  }

  // Legacy endpoint.
  // Returns a columnar-oriented one-shot result. Very inefficient for large
  // result sets. Very inefficient in general too.
  if (req.uri == "/raw_query") {
    std::vector<uint8_t> response = trace_processor_rpc_.RawQuery(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    return HttpReply(client->sock.get(), "200 OK", headers, response.data(),
                     response.size());
  }

  if (req.uri == "/status") {
    auto status = trace_processor_rpc_.GetStatus();
    return HttpReply(client->sock.get(), "200 OK", headers, status.data(),
                     status.size());
  }

  if (req.uri == "/compute_metric") {
    std::vector<uint8_t> res = trace_processor_rpc_.ComputeMetric(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    return HttpReply(client->sock.get(), "200 OK", headers, res.data(),
                     res.size());
  }

  if (req.uri == "/enable_metatrace") {
    trace_processor_rpc_.EnableMetatrace();
    return HttpReply(client->sock.get(), "200 OK", headers);
  }

  if (req.uri == "/disable_and_read_metatrace") {
    std::vector<uint8_t> res = trace_processor_rpc_.DisableAndReadMetatrace();
    return HttpReply(client->sock.get(), "200 OK", headers, res.data(),
                     res.size());
  }

  return HttpReply(client->sock.get(), "404 Not Found", headers);
}

}  // namespace

void RunHttpRPCServer(std::unique_ptr<TraceProcessor> preloaded_instance,
                      std::string port_number) {
  HttpServer srv(std::move(preloaded_instance));
  g_httpd_instance = &srv;
  std::string port = port_number.empty() ? kBindPort : port_number;
  std::string ipv4_addr = "127.0.0.1:" + port;
  std::string ipv6_addr = "[::1]:" + port;
  srv.Run(ipv4_addr.c_str(), ipv6_addr.c_str());
}

void HttpServer::ServeHelpPage(Client* client) {
  static const char kPage[] = R"(Perfetto Trace Processor RPC Server


This service can be used in two ways:

1. Open or reload https://ui.perfetto.dev/

It will automatically try to connect and use the server on localhost:9001 when
available. Click YES when prompted to use Trace Processor Native Acceleration
in the UI dialog.
See https://perfetto.dev/docs/visualization/large-traces for more.


2. Python API.

Example: perfetto.TraceProcessor(addr='localhost:9001')
See https://perfetto.dev/docs/analysis/trace-processor#python-api for more.


For questions:
https://perfetto.dev/docs/contributing/getting-started#community
)";

  char content_length[255];
  sprintf(content_length, "Content-Length: %zu", sizeof(kPage) - 1);
  std::initializer_list<const char*> headers{"Content-Type: text/plain",
                                             content_length};
  HttpReply(client->sock.get(), "200 OK", headers,
            reinterpret_cast<const uint8_t*>(kPage), sizeof(kPage) - 1);
}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // PERFETTO_TP_HTTPD
