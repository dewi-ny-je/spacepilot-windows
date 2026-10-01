#pragma once
#include "axial/web.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+" " #x);}while(0)
namespace net=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
namespace ws=beast::websocket;
namespace json=boost::json;
using tcp=net::ip::tcp;
std::string text(const json::value& v){return std::string(v.as_string());}
struct Client {
    net::io_context io;
    net::ssl::context tls{net::ssl::context::tls_client};
    ws::stream<beast::ssl_stream<beast::tcp_stream>> socket{io,tls};
    std::atomic<int> matrices{0};int transaction=0;bool moving=false;
    json::array matrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,10,1};
    std::string host;
    Client(unsigned port,const std::string& root):host("127.0.0.1:"+std::to_string(port)){
        tls.load_verify_file(root);socket.next_layer().set_verify_mode(net::ssl::verify_peer);
        socket.next_layer().set_verify_callback(net::ssl::host_name_verification("127.51.68.120"));
        beast::get_lowest_layer(socket).connect(tcp::endpoint(net::ip::make_address("127.0.0.1"),port));
        socket.next_layer().handshake(net::ssl::stream_base::client);
    }
    ~Client(){boost::system::error_code ec;beast::get_lowest_layer(socket).socket().close(ec);}
    void connect(){
        socket.set_option(ws::stream_base::decorator([](ws::request_type& r){r.set(http::field::origin,"https://any-cad.example");r.set(http::field::sec_websocket_protocol,"wamp");}));
        socket.handshake(host,"/");CHECK(read()[0]==0);
        send(json::array{1,"rpc","wss://127.51.68.120/3dconnexion#"});
    }
    void send(json::value value){socket.write(net::buffer(json::serialize(value)));}
    json::array read(){beast::flat_buffer buffer;socket.read(buffer);return json::parse(beast::buffers_to_string(buffer.data())).as_array();}
    void callback(const json::array& event){
        CHECK(event[0]==8);const auto& call=event[2].as_array();CHECK(call[0]==2);
        auto property=text(call[4]);json::value value;
        if(call[2]=="self:read"){
            if(property=="view.affine")value=matrix;
            else if(property=="view.perspective"||property=="view.rotatable")value=true;
            else if(property=="view.extents"||property=="model.extents")value=json::array{-1,-1,-1,1,1,1};
            else if(property=="view.target")value=json::array{0,0,0};
            else {send(json::array{4,call[1],"unsupported","Unsupported property"});return;}
        }else{
            CHECK(call[2]=="self:update");value=call[5];
            if(property=="motion")moving=value.as_bool();
            if(property=="transaction"){transaction+=value==1?1:-1;CHECK(transaction==0||transaction==1);}
            if(property=="view.affine"){CHECK(transaction==1);matrix=value.as_array();++matrices;}
        }
        send(json::array{3,call[1],value});
    }
    json::value call(json::array request){
        auto id=request[1];send(request);
        for(int i=0;i<100;++i){auto r=read();if(r[0]==8){callback(r);continue;}CHECK(r[0]==3&&r[1]==id);return r[2];}
        throw std::runtime_error("RPC did not complete");
    }
    void unknownController(const std::string& reference){
        send(json::array{2,"unknown-controller","rpc:read",reference,"focus"});
        auto error=read();CHECK(error[0]==4&&error[1]=="unknown-controller"&&error[3]=="Unknown controller");
    }
};
