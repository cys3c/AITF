#ifndef UDP_SERVER_HPP
#define UDP_SERVER_HPP
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include "../Managers/PacketManager.hpp"
#include "../logger.hpp"


class Udp_Server{

	public:
		static Udp_Server * getInstance();

		bool registerIP(uint32_t ip, PacketManager * pm);

		void start();
		void stop();

	private:
		Udp_Server();
		void handle_receive(const boost::system::error_code& error, std::size_t);
		void udp_listen();
		boost::asio::ip::udp::socket* udp_sock;
		boost::asio::ip::udp::endpoint remote_endpoint_;
		boost::array<uint8_t, 165> recv_buff;
		boost::asio::io_service udp_io;
		std::unordered_map<uint32_t, PacketManager *> reg_listens;

		static Udp_Server * udpserver;
};

#endif
