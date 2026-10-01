#pragma once
#include "core.hpp"
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace sn {
struct WebConfiguration {
    bool enabled=true;
    std::string certificate,key;
    std::map<std::string,std::array<std::string,32>> commands;
    bool operator==(const WebConfiguration&) const = default;
};
// Thread-safe facade: all network/session work belongs to a private I/O thread.
class WebServer {
public:
    explicit WebServer(std::string address="127.51.68.120",uint16_t port=8181);
    ~WebServer();
    void configure(WebConfiguration configuration);
    void retry();
    void receive(const Event& event);
    std::string status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
