#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <string>

#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>

namespace tcp_proxy {
    namespace ip = boost::asio::ip;

    class bridge : public boost::enable_shared_from_this<bridge> {
    public:
        typedef ip::tcp::socket socket_type;
        typedef boost::shared_ptr<bridge> ptr_type;

        bridge(boost::asio::io_service& ios)
            : downstream_socket_(ios),
              upstream_socket_(ios),
              resolver_(ios)
        {}

        socket_type& downstream_socket() {
            return downstream_socket_;
        }

        socket_type& upstream_socket() {
            return upstream_socket_;
        }

        void start(const std::string& upstream_host, unsigned short upstream_port) {
                                                                                         // host name asynchronously resolved 
            resolver_.async_resolve(upstream_host, std::to_string(upstream_port),
                boost::bind(&bridge::handle_resolve, shared_from_this(),
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::iterator));
        }

        void handle_resolve(const boost::system::error_code& error,
                            ip::tcp::resolver::iterator endpoint_iterator) {
            if (!error) {
                // Successfully resolved the host name, now attempt connection to remote server
                boost::asio::async_connect(upstream_socket_, endpoint_iterator,
                    boost::bind(&bridge::handle_upstream_connect, shared_from_this(),
                        boost::asio::placeholders::error));
            } else {
                std::cerr << "Error resolving upstream host: " << error.message() << std::endl;
                close();
            }
        }

        void handle_upstream_connect(const boost::system::error_code& error) {
            if (!error) {
                // Setup async read from remote server (upstream)
                upstream_socket_.async_read_some(
                    boost::asio::buffer(upstream_data_, max_data_length),
                    boost::bind(&bridge::handle_upstream_read,
                        shared_from_this(),
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));

                // Setup async read from client (downstream)
                downstream_socket_.async_read_some(
                    boost::asio::buffer(downstream_data_, max_data_length),
                    boost::bind(&bridge::handle_downstream_read,
                        shared_from_this(),
                        boost::asio::placeholders::error,
                        boost::asio::placeholders::bytes_transferred));
            } else {
                std::cerr << "Error connecting to upstream: " << error.message() << std::endl;
                close();
            }
        }

        // Remaining methods remain unchanged...

    private:
        void close() {
            boost::mutex::scoped_lock lock(mutex_);

            if (downstream_socket_.is_open()) {
                downstream_socket_.close();
            }

            if (upstream_socket_.is_open()) {
                upstream_socket_.close();
            }
        }

        // Existing members...
        ip::tcp::resolver resolver_;
        // Existing methods...

        socket_type downstream_socket_;
        socket_type upstream_socket_;

        enum { max_data_length = 8192 }; //8KB
        unsigned char downstream_data_[max_data_length];
        unsigned char upstream_data_[max_data_length];

        boost::mutex mutex_;
    };

    class acceptor {
    public:
        acceptor(boost::asio::io_service& io_service,
                 const std::string& local_host, unsigned short local_port,
                 const std::string& upstream_host, unsigned short upstream_port)
            : io_service_(io_service),
              localhost_address(ip::address_v4::from_string(local_host)),
              acceptor_(io_service_, ip::tcp::endpoint(localhost_address, local_port)),
              upstream_port_(upstream_port),
              upstream_host_(upstream_host)
        {}

        bool accept_connections() {
            try {
                session_ = boost::shared_ptr<bridge>(new bridge(io_service_));

                acceptor_.async_accept(session_->downstream_socket(),
                    boost::bind(&acceptor::handle_accept,
                        this,
                        boost::asio::placeholders::error));
            } catch (std::exception& e) {
                std::cerr << "acceptor exception: " << e.what() << std::endl;
                return false;
            }

            return true;
        }

    private:
        void handle_accept(const boost::system::error_code& error) {
            if (!error) {
                session_->start(upstream_host_, upstream_port_);

                if (!accept_connections()) {
                    std::cerr << "Failure during call to accept." << std::endl;
                }
            } else {
                std::cerr << "Error: " << error.message() << std::endl;
            }
        }

        boost::asio::io_service& io_service_;
        ip::address_v4 localhost_address;
        ip::tcp::acceptor acceptor_;
        tcp_proxy::bridge::ptr_type session_;
        unsigned short upstream_port_;
        std::string upstream_host_;
    };
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "usage: tcpproxy_server <local host ip> <local port> <forward host ip> <forward port>" << std::endl;
        return 1;
    }

    const unsigned short local_port = static_cast<unsigned short>(::atoi(argv[2]));
    const unsigned short forward_port = static_cast<unsigned short>(::atoi(argv[4]));
    const std::string local_host = argv[1];
    const std::string forward_host = argv[3];

    boost::asio::io_service ios;

    try {
        tcp_proxy::acceptor acceptor(ios,
            local_host, local_port,
            forward_host, forward_port);

        acceptor.accept_connections();

        ios.run();
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
